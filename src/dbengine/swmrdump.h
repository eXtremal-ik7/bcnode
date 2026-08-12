#pragma once

// Sparse-raw dump of CSwmrCache: per-bucket groups of raw records, chunked
// by fixed bucket ranges, each chunk independently checksummed. Save
// parallelizes chunk building over disjoint bucket ranges and writes chunks
// in completion order (the chunk table keeps id -> file offset); load walks
// chunks in file-offset order so a spinning disk always seeks forward. All
// file I/O is positional (FileDescriptor): the loader shares one descriptor
// between worker threads, the saver writes chunk payloads at explicit
// offsets and finalizes header + chunk table with one write at offset 0.
// The header pins a codec id; this revision writes and accepts only codec 0
// (raw) - compression can slot in later without a format break.
//
// Rejection policy: a cache dump is a pure warmup optimization, absence is
// always legal, so ANY mismatch - magic, format or value version, table
// geometry, stamp, checksum, a single key that does not map to its bucket -
// discards the whole dump and leaves the cache empty. There are no partial
// loads and no migrations. The format is machine-local by design (native
// endianness, in-memory ValueT layout incl. padding); the header pins every
// parameter so a difference is a clean rejection, never a misread.
//
// The stamp is the semantic integrity check: the caller records the block
// (height, hash) the cache content corresponds to - it must equal the
// database stamp written by the same flush - and load() rejects the file
// unless the expected stamp matches exactly. A bit-perfect dump at the
// wrong height is more dangerous than a corrupt one: it would resurrect
// spent outputs as false positives, breaking "a positive must be exact".
//
// File layout:
//   SSwmrDumpHeader (128 B, self-checksummed)
//   SSwmrChunkRef[ChunkCount], then uint64 hash of the table
//   chunk payloads at arbitrary offsets (completion order)
//
// Chunk id covers buckets [id*BucketsPerChunk, min((id+1)*BucketsPerChunk,
// NumBuckets)). Chunk payload:
//   repeated { uint32 bucketIdx; uint32 count; count * record }
// with strictly increasing bucketIdx confined to the chunk range;
//   record = txid[32] | uint32 vout | raw ValueT bytes
// Condemned entries are dumped like any others: the floor restarts at zero
// on load and the next maintain() re-derives it from the same population.

#include "dbengine/swmrcache.h"
#include "common/file.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace dbengine {

struct SSwmrDumpStamp {
  uint64_t Height = 0;
  uint8_t BlockHash[32] = {};
};

struct SSwmrDumpHeader {
  static constexpr uint64_t MAGIC = 0x504D44434F585455ull; // "UTXOCDMP"
  static constexpr uint32_t FORMAT_VERSION = 1;

  uint64_t Magic;
  uint32_t FormatVersion;
  uint32_t HeaderSize;
  uint32_t ValueVersion;   // caller-defined ValueT layout version
  uint32_t ValueSize;      // sizeof(ValueT)
  uint32_t SlotsPerBucket;
  uint32_t Codec;          // 0 = raw; reserved for future compressed formats
  uint64_t CacheLimit;
  uint64_t NumBuckets;
  uint64_t EntryCount;
  uint64_t StampHeight;
  uint8_t StampHash[32];
  uint32_t BucketsPerChunk;
  uint32_t ChunkCount;
  uint8_t Reserved[16];
  uint64_t HeaderHash;     // swmrDumpHash64 of the preceding 120 bytes
};

static_assert(sizeof(SSwmrDumpHeader) == 128 && std::is_trivially_copyable_v<SSwmrDumpHeader>);

struct SSwmrChunkRef {
  uint64_t Offset;
  uint64_t RawSize;
  uint64_t StoredSize;     // equals RawSize while the only codec is raw
  uint64_t Hash;           // swmrDumpHash64 of the stored bytes
};

static_assert(sizeof(SSwmrChunkRef) == 32 && std::is_trivially_copyable_v<SSwmrChunkRef>);

struct SSwmrDumpSaveOptions {
  uint32_t ValueVersion = 1;
  uint32_t BucketsPerChunk = 8192;
  unsigned Threads = 1;
};

struct SSwmrDumpLoadOptions {
  uint32_t ValueVersion = 1;
  unsigned Threads = 1;
};

// 4-lane 64-bit mixing hash over 32-byte stripes: a format-local checksum
// for error detection, not a standard digest and not collision-resistant
// against an adversary (who could rewrite the database and the binary just
// as easily). Deterministic on a given machine, ~1 word/lane/cycle.
inline uint64_t swmrDumpHash64(const void *data, size_t size)
{
  constexpr uint64_t P1 = 0x9E3779B185EBCA87ull;
  constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4Full;
  constexpr uint64_t P3 = 0x165667B19E3779F9ull;
  const uint8_t *p = static_cast<const uint8_t*>(data);
  uint64_t acc[4] = {P1, P2, P3, P1 ^ P2};
  auto rotl = [](uint64_t x, unsigned r) { return (x << r) | (x >> (64u - r)); };
  auto round = [&acc, &rotl](unsigned i, uint64_t v) { acc[i] = rotl(acc[i] + v * P2, 31) * P1; };

  size_t remaining = size;
  while (remaining >= 32) {
    for (unsigned i = 0; i < 4; i++) {
      uint64_t v;
      memcpy(&v, p + 8 * i, 8);
      round(i, v);
    }
    p += 32;
    remaining -= 32;
  }
  if (remaining) {
    uint8_t tail[32] = {};
    memcpy(tail, p, remaining);
    for (unsigned i = 0; i < 4; i++) {
      uint64_t v;
      memcpy(&v, tail + 8 * i, 8);
      round(i, v);
    }
  }

  uint64_t h = rotl(acc[0], 1) + rotl(acc[1], 7) + rotl(acc[2], 12) + rotl(acc[3], 18) + size;
  for (unsigned i = 0; i < 4; i++) {
    h ^= acc[i];
    h = h * P1 + P3;
  }
  h ^= h >> 30;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 27;
  h *= 0x94D049BB133111EBull;
  h ^= h >> 31;
  return h;
}

namespace SwmrDumpDetail {

inline void setError(std::string *error, std::string message)
{
  if (error)
    *error = std::move(message);
}

inline bool readHeader(FileDescriptor &fd, SSwmrDumpHeader &header, std::string *error)
{
  if (fd.read(&header, 0, sizeof(header)) != static_cast<ssize_t>(sizeof(header))) {
    setError(error, "dump rejected: truncated header");
    return false;
  }
  if (header.Magic != SSwmrDumpHeader::MAGIC) {
    setError(error, "dump rejected: bad magic");
    return false;
  }
  if (swmrDumpHash64(&header, offsetof(SSwmrDumpHeader, HeaderHash)) != header.HeaderHash) {
    setError(error, "dump rejected: header checksum mismatch");
    return false;
  }
  if (header.FormatVersion != SSwmrDumpHeader::FORMAT_VERSION || header.HeaderSize != sizeof(SSwmrDumpHeader)) {
    setError(error, "dump rejected: format version mismatch");
    return false;
  }
  return true;
}

struct SReadyChunk {
  uint32_t Id = 0;
  uint64_t EntryCount = 0;
  uint64_t Hash = 0;
  std::vector<uint8_t> Raw;
};

// bounded completion queue between chunk builders and the file writer;
// a raised failure flag unblocks everybody
class CReadyQueue {
private:
  std::mutex Mutex_;
  std::condition_variable CanPush_;
  std::condition_variable CanPop_;
  std::deque<SReadyChunk> Items_;
  size_t Limit_;

public:
  explicit CReadyQueue(size_t limit) : Limit_(limit) {}

  bool push(SReadyChunk &&item, const std::atomic<bool> &failed) {
    std::unique_lock lock(Mutex_);
    CanPush_.wait(lock, [&]{ return Items_.size() < Limit_ || failed.load(); });
    if (failed.load())
      return false;
    Items_.push_back(std::move(item));
    CanPop_.notify_one();
    return true;
  }

  bool pop(SReadyChunk &item, const std::atomic<bool> &failed) {
    std::unique_lock lock(Mutex_);
    CanPop_.wait(lock, [&]{ return !Items_.empty() || failed.load(); });
    if (Items_.empty())
      return false;
    item = std::move(Items_.front());
    Items_.pop_front();
    CanPush_.notify_one();
    return true;
  }

  void wakeAll() {
    { std::lock_guard lock(Mutex_); }
    CanPush_.notify_all();
    CanPop_.notify_all();
  }
};

} // namespace SwmrDumpDetail

// reads and verifies the header only: lets the caller compare the stamp
// against the database before constructing a cache of the dumped geometry
inline bool swmrDumpPeek(const std::filesystem::path &path, SSwmrDumpHeader &header, std::string *error = nullptr)
{
  // FileDescriptor::open creates missing files, so probe existence first
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    SwmrDumpDetail::setError(error, "can't open " + path.string());
    return false;
  }
  FileDescriptor fd;
  if (!fd.open(path)) {
    SwmrDumpDetail::setError(error, "can't open " + path.string());
    return false;
  }
  bool ok = SwmrDumpDetail::readHeader(fd, header, error);
  fd.close();
  return ok;
}

// Writes the dump to <path>.tmp and renames it over <path> on success, so a
// crash mid-save leaves either the old dump or a complete new one. Worker
// threads build/hash chunks, the calling thread writes whichever chunk is
// ready next at the running offset; header and chunk table land last, so an
// unfinished .tmp never carries a valid magic. The cache must be quiescent
// (post-flush, pre-readers).
template<typename ValueT>
bool swmrDumpSave(const CSwmrCache<ValueT> &cache,
                  const std::filesystem::path &path,
                  const SSwmrDumpStamp &stamp,
                  const SSwmrDumpSaveOptions &options,
                  std::string *error = nullptr)
{
  using Cache = CSwmrCache<ValueT>;
  constexpr uint32_t recordSize = 36 + sizeof(ValueT);

  const size_t numBuckets = cache.numBuckets();
  const uint32_t bucketsPerChunk = options.BucketsPerChunk ? options.BucketsPerChunk : 8192;
  const uint32_t chunkCount = static_cast<uint32_t>((numBuckets + bucketsPerChunk - 1) / bucketsPerChunk);
  const unsigned threads = std::max(options.Threads, 1u);

  std::filesystem::path tmpPath = path;
  tmpPath += ".tmp";
  FileDescriptor fd;
  if (!fd.open(tmpPath) || !fd.truncate(0)) {
    SwmrDumpDetail::setError(error, "can't create " + tmpPath.string());
    return false;
  }

  const uint64_t dataOffset = sizeof(SSwmrDumpHeader) + static_cast<uint64_t>(chunkCount) * sizeof(SSwmrChunkRef) + 8;
  std::atomic<bool> failed{false};
  std::mutex errorMutex;
  SwmrDumpDetail::CReadyQueue queue(threads + 2);
  auto fail = [&](std::string message) {
    std::lock_guard lock(errorMutex);
    if (!failed.exchange(true))
      SwmrDumpDetail::setError(error, std::move(message));
    queue.wakeAll();
  };

  std::vector<SSwmrChunkRef> table(chunkCount);
  std::atomic<uint32_t> nextChunk{0};

  auto workerFn = [&]() {
    typename Cache::SExportEntry entries[Cache::SLOTS_PER_BUCKET];
    for (;;) {
      if (failed.load(std::memory_order_relaxed))
        return;
      uint32_t id = nextChunk.fetch_add(1);
      if (id >= chunkCount)
        return;
      size_t from = static_cast<size_t>(id) * bucketsPerChunk;
      size_t to = std::min(from + bucketsPerChunk, numBuckets);

      SwmrDumpDetail::SReadyChunk ready;
      ready.Id = id;
      std::vector<uint8_t> &raw = ready.Raw;
      for (size_t b = from; b < to; b++) {
        unsigned n = cache.exportBucket(b, entries);
        if (!n)
          continue;
        size_t pos = raw.size();
        raw.resize(pos + 8 + static_cast<size_t>(n) * recordSize);
        uint8_t *out = raw.data() + pos;
        uint32_t b32 = static_cast<uint32_t>(b);
        memcpy(out, &b32, 4);
        memcpy(out + 4, &n, 4);
        out += 8;
        for (unsigned i = 0; i < n; i++, out += recordSize) {
          memcpy(out, entries[i].TxId, 32);
          memcpy(out + 32, &entries[i].Vout, 4);
          memcpy(out + 36, &entries[i].Value, sizeof(ValueT));
        }
        ready.EntryCount += n;
      }

      ready.Hash = swmrDumpHash64(raw.data(), raw.size());
      if (!queue.push(std::move(ready), failed))
        return;
    }
  };

  std::vector<std::thread> workers;
  for (unsigned i = 0; i < threads; i++)
    workers.emplace_back(workerFn);

  uint64_t offset = dataOffset;
  uint64_t totalEntries = 0;
  for (uint32_t written = 0; written < chunkCount; written++) {
    SwmrDumpDetail::SReadyChunk ready;
    if (!queue.pop(ready, failed))
      break;
    SSwmrChunkRef &ref = table[ready.Id];
    ref.Offset = offset;
    ref.RawSize = ready.Raw.size();
    ref.StoredSize = ready.Raw.size();
    ref.Hash = ready.Hash;
    if (!ready.Raw.empty() && fd.write(ready.Raw.data(), offset, ready.Raw.size()) != static_cast<ssize_t>(ready.Raw.size())) {
      fail("write failed (disk full?)");
      break;
    }
    offset += ready.Raw.size();
    totalEntries += ready.EntryCount;
  }
  for (auto &worker: workers)
    worker.join();

  bool ok = !failed.load();
  if (ok && totalEntries != cache.size()) {
    SwmrDumpDetail::setError(error, "cache mutated during dump");
    ok = false;
  }

  if (ok) {
    SSwmrDumpHeader header{};
    header.Magic = SSwmrDumpHeader::MAGIC;
    header.FormatVersion = SSwmrDumpHeader::FORMAT_VERSION;
    header.HeaderSize = sizeof(SSwmrDumpHeader);
    header.ValueVersion = options.ValueVersion;
    header.ValueSize = sizeof(ValueT);
    header.SlotsPerBucket = Cache::SLOTS_PER_BUCKET;
    header.Codec = 0;
    header.CacheLimit = cache.limit();
    header.NumBuckets = numBuckets;
    header.EntryCount = totalEntries;
    header.StampHeight = stamp.Height;
    memcpy(header.StampHash, stamp.BlockHash, 32);
    header.BucketsPerChunk = bucketsPerChunk;
    header.ChunkCount = chunkCount;
    header.HeaderHash = swmrDumpHash64(&header, offsetof(SSwmrDumpHeader, HeaderHash));
    uint64_t tableHash = swmrDumpHash64(table.data(), table.size() * sizeof(SSwmrChunkRef));

    // header + chunk table + table hash in one write at offset 0
    std::vector<uint8_t> head(dataOffset);
    memcpy(head.data(), &header, sizeof(header));
    memcpy(head.data() + sizeof(header), table.data(), table.size() * sizeof(SSwmrChunkRef));
    memcpy(head.data() + sizeof(header) + table.size() * sizeof(SSwmrChunkRef), &tableHash, 8);
    ok = fd.write(head.data(), 0, head.size()) == static_cast<ssize_t>(head.size());
    if (!ok)
      SwmrDumpDetail::setError(error, "finalize failed: can't write header");
  }

  fd.close();
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    return false;
  }

  std::error_code ec;
  std::filesystem::rename(tmpPath, path, ec);
  if (ec) {
    SwmrDumpDetail::setError(error, "rename failed: " + ec.message());
    std::filesystem::remove(tmpPath, ec);
    return false;
  }
  return true;
}

// Loads a dump into an empty cache constructed with the same config. On any
// rejection the cache is cleared back to empty and false is returned: the
// caller proceeds with a cold start. Worker threads read disjoint chunks
// through one shared descriptor (positional reads) in file-offset order and
// place buckets directly (importBucket), then a single scan rebuilds the
// counters.
template<typename ValueT>
bool swmrDumpLoad(CSwmrCache<ValueT> &cache,
                  const std::filesystem::path &path,
                  const SSwmrDumpStamp &expectedStamp,
                  const SSwmrDumpLoadOptions &options,
                  std::string *error = nullptr)
{
  using Cache = CSwmrCache<ValueT>;
  constexpr uint32_t recordSize = 36 + sizeof(ValueT);

  if (cache.size() != 0) {
    SwmrDumpDetail::setError(error, "target cache is not empty");
    return false;
  }

  // FileDescriptor::open creates missing files, so probe existence first
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    SwmrDumpDetail::setError(error, "can't open " + path.string());
    return false;
  }

  FileDescriptor fd;
  if (!fd.open(path)) {
    SwmrDumpDetail::setError(error, "can't open " + path.string());
    return false;
  }
  const uint64_t fileSize = fd.size();

  SSwmrDumpHeader header;
  std::vector<SSwmrChunkRef> table;
  {
    auto reject = [&](const char *what) {
      SwmrDumpDetail::setError(error, std::string("dump rejected: ") + what);
      fd.close();
      return false;
    };

    if (!SwmrDumpDetail::readHeader(fd, header, error)) {
      fd.close();
      return false;
    }
    if (header.ValueVersion != options.ValueVersion)
      return reject("value version mismatch");
    if (header.ValueSize != sizeof(ValueT))
      return reject("value size mismatch");
    if (header.SlotsPerBucket != Cache::SLOTS_PER_BUCKET)
      return reject("slots per bucket mismatch");
    if (header.CacheLimit != cache.limit())
      return reject("cache size limit mismatch");
    if (header.NumBuckets != cache.numBuckets())
      return reject("table geometry mismatch");
    if (header.Codec != 0)
      return reject("unsupported codec");
    if (!header.BucketsPerChunk ||
        header.ChunkCount != (header.NumBuckets + header.BucketsPerChunk - 1) / header.BucketsPerChunk)
      return reject("chunk layout mismatch");
    if (header.EntryCount > header.NumBuckets * Cache::SLOTS_PER_BUCKET)
      return reject("entry count out of range");
    if (header.StampHeight != expectedStamp.Height || memcmp(header.StampHash, expectedStamp.BlockHash, 32) != 0)
      return reject("stamp mismatch");

    table.resize(header.ChunkCount);
    const size_t tableBytes = table.size() * sizeof(SSwmrChunkRef);
    uint64_t tableHash = 0;
    if (fd.read(table.data(), sizeof(SSwmrDumpHeader), tableBytes) != static_cast<ssize_t>(tableBytes) ||
        fd.read(&tableHash, sizeof(SSwmrDumpHeader) + tableBytes, 8) != 8)
      return reject("truncated chunk table");
    if (swmrDumpHash64(table.data(), tableBytes) != tableHash)
      return reject("chunk table checksum mismatch");

    const uint64_t dataOffset = sizeof(SSwmrDumpHeader) + tableBytes + 8;
    const uint64_t maxBucketBytes = 8 + static_cast<uint64_t>(Cache::SLOTS_PER_BUCKET) * recordSize;
    for (uint32_t id = 0; id < header.ChunkCount; id++) {
      const SSwmrChunkRef &ref = table[id];
      size_t from = static_cast<size_t>(id) * header.BucketsPerChunk;
      size_t to = std::min<size_t>(from + header.BucketsPerChunk, header.NumBuckets);
      if (ref.Offset < dataOffset || ref.StoredSize > fileSize || ref.Offset > fileSize - ref.StoredSize)
        return reject("chunk outside the file");
      if (ref.RawSize > (to - from) * maxBucketBytes)
        return reject("chunk larger than its bucket range");
      if (ref.StoredSize != ref.RawSize)
        return reject("stored/raw size mismatch");
    }
  }

  // walk chunks in file-offset order: with a shuffled (completion-order)
  // layout this keeps a spinning disk moving forward
  std::vector<uint32_t> order(header.ChunkCount);
  for (uint32_t i = 0; i < header.ChunkCount; i++)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return table[a].Offset < table[b].Offset; });

  const unsigned threads = std::max(1u, std::min(options.Threads, header.ChunkCount));
  std::atomic<bool> failed{false};
  std::mutex errorMutex;
  auto fail = [&](std::string message) {
    std::lock_guard lock(errorMutex);
    if (!failed.exchange(true))
      SwmrDumpDetail::setError(error, std::move(message));
  };
  std::atomic<uint32_t> nextIndex{0};
  std::atomic<uint64_t> totalEntries{0};

  auto workerFn = [&]() {
    typename Cache::SExportEntry entries[Cache::SLOTS_PER_BUCKET];
    std::vector<uint8_t> raw;

    for (;;) {
      if (failed.load(std::memory_order_relaxed))
        break;
      uint32_t index = nextIndex.fetch_add(1);
      if (index >= header.ChunkCount)
        break;
      const uint32_t id = order[index];
      const SSwmrChunkRef &ref = table[id];
      const size_t from = static_cast<size_t>(id) * header.BucketsPerChunk;
      const size_t to = std::min<size_t>(from + header.BucketsPerChunk, header.NumBuckets);

      raw.resize(ref.StoredSize);
      if (ref.StoredSize) {
        if (fd.read(raw.data(), ref.Offset, raw.size()) != static_cast<ssize_t>(raw.size())) {
          fail("chunk read failed");
          break;
        }
      }
      if (swmrDumpHash64(raw.data(), raw.size()) != ref.Hash) {
        fail("chunk checksum mismatch");
        break;
      }

      size_t pos = 0;
      int64_t prevBucket = static_cast<int64_t>(from) - 1;
      uint64_t count = 0;
      bool parseOk = true;
      while (pos < raw.size()) {
        if (raw.size() - pos < 8) {
          parseOk = false;
          break;
        }
        uint32_t bucket, n;
        memcpy(&bucket, raw.data() + pos, 4);
        memcpy(&n, raw.data() + pos + 4, 4);
        if (bucket < from || bucket >= to || static_cast<int64_t>(bucket) <= prevBucket ||
            n == 0 || n > Cache::SLOTS_PER_BUCKET ||
            raw.size() - pos - 8 < static_cast<uint64_t>(n) * recordSize) {
          parseOk = false;
          break;
        }
        pos += 8;
        for (unsigned i = 0; i < n; i++, pos += recordSize) {
          memcpy(entries[i].TxId, raw.data() + pos, 32);
          memcpy(&entries[i].Vout, raw.data() + pos + 32, 4);
          memcpy(&entries[i].Value, raw.data() + pos + 36, sizeof(ValueT));
        }
        if (!cache.importBucket(bucket, entries, n)) {
          parseOk = false;
          break;
        }
        prevBucket = bucket;
        count += n;
      }
      if (!parseOk) {
        fail("chunk payload malformed");
        break;
      }
      totalEntries.fetch_add(count);
    }
  };

  std::vector<std::thread> workers;
  for (unsigned i = 0; i < threads; i++)
    workers.emplace_back(workerFn);
  for (auto &worker: workers)
    worker.join();
  fd.close();

  if (failed.load() || totalEntries.load() != header.EntryCount) {
    if (!failed.load())
      SwmrDumpDetail::setError(error, "dump rejected: entry count mismatch");
    cache.clear();
    return false;
  }

  cache.finalizeImport();
  if (cache.size() != header.EntryCount) {
    SwmrDumpDetail::setError(error, "dump rejected: post-import accounting mismatch");
    cache.clear();
    return false;
  }
  return true;
}

}
