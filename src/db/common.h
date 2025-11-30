// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/blockDataBase.h"
#include "common/mlog.h"

#include "config4cpp/Configuration.h"
#include <rocksdb/db.h>
#include <rocksdb/merge_operator.h>
#include "tbb/concurrent_hash_map.h"

namespace BC {
namespace DB {

struct CShardEntry {
  CShardEntry(const BC::Proto::BlockHashTy &blockId, void *keyData) : BlockId(blockId), KeyData(keyData) {}
  BC::Proto::BlockHashTy BlockId;
  void *KeyData;
};

struct CEntry {
  BC::Proto::BlockHashTy BlockId;
  bool IsConnected;
  CEntry() {}
  CEntry(const BC::Proto::BlockHashTy &blockId, bool isConnected) : BlockId(blockId), IsConnected(isConnected) {}
};

template<typename CKey>
struct CLog {
  MLog Log;
  std::unordered_map<CKey, std::vector<CShardEntry>> AllKeysData;
  tbb::concurrent_hash_map<CKey, void*> CurrentKeysData;

  const void *find(const CKey &key) {
    typename decltype (CurrentKeysData)::const_accessor accessor;
    if (CurrentKeysData.find(accessor, key))
      return accessor->second;
    return nullptr;
  }

  void update(const BC::Proto::BlockHashTy &blockId, const CKey &key, void *newData) {
    std::vector<CShardEntry> &blocks = AllKeysData[key];
    if (blocks.empty() || blocks.back().BlockId != blockId) {
      blocks.emplace_back(blockId, newData);
    } else {
      blocks.back().KeyData = newData;
    }

    {
      typename decltype (CurrentKeysData)::accessor accessor;
      CurrentKeysData.insert(accessor, key);
      accessor->second = newData;
    }
  }

  void pop(const BC::Proto::BlockHashTy &blockId) {
    for (auto &keyData: AllKeysData) {
      if (keyData.second.back().BlockId == blockId) {
        keyData.second.pop_back();
        if (keyData.second.empty()) {
          CurrentKeysData.erase(keyData.first);
        } else {
          typename decltype (CurrentKeysData)::accessor accessor;
          CurrentKeysData.insert(accessor, keyData.first);
          accessor->second = keyData.second.back().KeyData;
        }
      }
    }
  }

  void reset() {
    AllKeysData.clear();
    CurrentKeysData.clear();
    Log.reset();
  }
};

#pragma pack(push, 1)
struct CBaseCfg {
  uint32_t Version;
  uint32_t ShardsNum;
};
#pragma pack(pop)

class BaseInterface {
public:
  virtual ~BaseInterface() {}

  virtual rocksdb::MergeOperator *mergeOperator() = 0;

  virtual bool initialize(BlockInMemoryIndex &blockIndex,
                          BlockDatabase &blockDb,
                          std::filesystem::path &dataDir,
                          BC::DB::Storage &storage,
                          config4cpp::Configuration *cfg,
                          BC::Common::BlockIndex **forConnect,
                          std::vector<BC::Common::BlockIndex*> &forDisconnect) = 0;
  virtual void connect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) = 0;
  virtual void disconnect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) = 0;
  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) = 0;
  virtual void *interface(int interface) = 0;

  BC::Proto::BlockHashTy currentBlock() { return CurrentBlock_; }
  virtual void flush() = 0;

protected:
  BC::Proto::BlockHashTy CurrentBlock_;
};

template<typename CKey>
class Base : public BaseInterface {
public:
  Base(const std::string &name) : Name_(name) {}
  virtual ~Base() {}

  bool initialize(BlockInMemoryIndex &blockIndex,
                  BlockDatabase &blockDb,
                  std::filesystem::path &dataDir,
                  BC::DB::Storage &storage,
                  config4cpp::Configuration *cfg,
                  BC::Common::BlockIndex **forConnect,
                  std::vector<BC::Common::BlockIndex*> &forDisconnect) final {
    BaseCfg_.ShardsNum = static_cast<unsigned>(cfg->lookupInt(Name_.c_str(), "shardsNum", 1));
    BaseCfg_.Version = version();

    BC::Common::BlockIndex *bestIndex = blockIndex.best();
    Shards_.reset(new CLog<CKey>[BaseCfg_.ShardsNum]);
    OnDiskStorage_.resize(BaseCfg_.ShardsNum);
    *forConnect = nullptr;

    // Open all shards
    BC::Proto::BlockHashTy stamp;
    bool needConnectGenesis = false;
    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      auto shardPath = dataDir / Name_ / std::to_string(i);
      std::filesystem::create_directories(shardPath);

      rocksdb::DB *db;
      rocksdb::Options options;
      options.create_if_missing = true;
      options.merge_operator.reset(mergeOperator());
      rocksdb::Status status = rocksdb::DB::Open(options, shardPath.u8string().c_str(), &db);
      if (!status.ok()) {
        LOG_F(ERROR, "Can't open or create txdb database at %s", shardPath.u8string().c_str());
        return false;
      }

      OnDiskStorage_[i].reset(db);

      bool isEmpty = false;

      {
        // Check base configuration (shards num)
        rocksdb::Slice key("basecfg");
        std::string value;
        if (db->Get(rocksdb::ReadOptions(), key, &value).ok() && value.size() >= sizeof(CBaseCfg)) {
          const CBaseCfg *storedCfg = reinterpret_cast<const CBaseCfg*>(value.data());
          if (storedCfg->Version != BaseCfg_.Version) {
            LOG_F(ERROR, "database '%s' uses version %u, but found %u, restart with --reindex=%s",
                  Name_.c_str(), BaseCfg_.Version, storedCfg->Version, Name_.c_str());
            return false;
          }

          if (storedCfg->ShardsNum != BaseCfg_.ShardsNum) {
            LOG_F(ERROR, "database '%s configured with %u shards, found database with %u shards, restart with --reindex=%s",
                  Name_.c_str(), BaseCfg_.ShardsNum, storedCfg->ShardsNum, Name_.c_str());
            return false;
          }
        } else {
          // DB not have base configuration, write current to it
          rocksdb::Slice value(reinterpret_cast<char*>(&BaseCfg_), sizeof(BaseCfg_));
          db->Put(rocksdb::WriteOptions(), key, value);
          isEmpty = true;
        }
      }

      // Check stamp (last known block)
      std::string stampData;
      if (!isEmpty && db->Get(rocksdb::ReadOptions(), rocksdb::Slice("stamp"), &stampData).ok()) {
        if (stampData.size() != sizeof(BC::Proto::BlockHashTy)) {
          LOG_F(ERROR, "%s is corrupted: invalid stamp size (%s)", Name_.c_str(), shardPath.u8string().c_str());
          return false;
        }

        BC::Proto::BlockHashTy shardStamp;
        memcpy(shardStamp.begin(), stampData.data(), sizeof(BC::Proto::BlockHashTy));
        if (i == 0) {
          stamp = shardStamp;
          auto It = blockIndex.blockIndex().find(stamp);
          if (It == blockIndex.blockIndex().end()) {
            LOG_F(ERROR,
                  "%s is corrupted: stamp %s not exists in block index (%s)",
                  Name_.c_str(),
                  stamp.GetHex().c_str(),
                  shardPath.u8string().c_str());
            return false;
          }

          // Build connect and disconnect block set if need
          *forConnect = rebaseChain(bestIndex, It->second, forDisconnect);
        } else if (shardStamp != stamp) {
          LOG_F(ERROR, "%s is corrupted: shard %u has different stamp", Name_.c_str(), i);
          return false;
        }
      } else {
        // database is empty, run full rescanning
        *forConnect = blockIndex.genesis();
      }
    }

    if (!initializeImpl(cfg, storage))
      return false;
    if (needConnectGenesis)
      connect(blockIndex.genesis(), blockIndex.genesisBlock(), blockIndex, blockDb);
    return true;
  }

  void connect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) final {
    auto hash = index->Header.GetHash();

    if (!PendingBlocks_.empty()) {
      auto last = PendingBlocks_.back();
      if (!last.IsConnected && last.BlockId == hash) {
        for (size_t i = 0; i < BaseCfg_.ShardsNum; i++)
          Shards_[i].pop(hash);
        return;
      }
    }

    connectImpl(index, block, blockIndex, blockDb);
    PendingBlocks_.emplace_back(hash, true);
    CurrentBlock_ = hash;

    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      if (Shards_[i].Log.size() >= 1u << 24)
        flushImpl(CurrentBlock_, i);
    }
  }

  void disconnect(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) final {
    auto hash = index->Header.GetHash();

    if (!PendingBlocks_.empty()) {
      auto last = PendingBlocks_.back();
      if (last.IsConnected && last.BlockId == hash) {
        for (size_t i = 0; i < BaseCfg_.ShardsNum; i++)
          Shards_[i].pop(hash);
        return;
      }
    }

    disconnectImpl(index, block, blockIndex, blockDb);
    PendingBlocks_.emplace_back(hash, false);
    CurrentBlock_ = index->Header.hashPrevBlock;
  }

  void flush() final {
    for (size_t i = 0; i < BaseCfg_.ShardsNum; i++) {
      if (!Shards_[i].AllKeysData.empty())
        flushImpl(CurrentBlock_, i);
    }
  }

  BC::Proto::BlockHashTy currentBlock() { return CurrentBlock_; }

  virtual uint32_t version() = 0;
  virtual bool initializeImpl(config4cpp::Configuration *cfg, BC::DB::Storage &storage) = 0;
  virtual void connectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) = 0;
  virtual void disconnectImpl(const BC::Common::BlockIndex *index, const BC::Proto::Block &block, BlockInMemoryIndex &blockIndex, BlockDatabase &blockDb) = 0;

protected:
  // Configuration
  std::string Name_;
  CBaseCfg BaseCfg_;

  // Database structures
  std::vector<CEntry> PendingBlocks_;
  std::unique_ptr<CLog<CKey>[]> Shards_;
  std::vector<std::unique_ptr<rocksdb::DB>> OnDiskStorage_;
};

template<typename CKey>
class CBaseKV : public Base<CKey> {
private:
  struct CHeader {
    CKey Key;
    size_t Size;
    bool IsConnected;
  };

public:
  CBaseKV(const std::string &name) : Base<CKey>(name) {}
  virtual ~CBaseKV() {}
  rocksdb::MergeOperator *mergeOperator() final { return nullptr; }

  void add(const BC::Proto::BlockHashTy &blockId, const CKey &key, const void *data, size_t size) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->Shards_[hasher(key) % this->BaseCfg_.ShardsNum];

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + size));
    header->Key = key;
    header->Size = size;
    header->IsConnected = true;

    void *buffer = header + 1;
    memcpy(buffer, data, size);

    shard.update(blockId, key, header);
  }

  void remove(const BC::Proto::BlockHashTy &blockId, const CKey &key) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->Shards_[hasher(key) % this->BaseCfg_.ShardsNum];

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    header->Size = 0;
    header->IsConnected = false;

    shard.update(blockId, key, header);
  }

  bool find(const CKey &key, std::function<void(const void*, size_t)> callback) const {
    std::hash<CKey> hasher;
    size_t shardIndex = hasher(key) % this->BaseCfg_.ShardsNum;
    auto &shard = this->Shards_[shardIndex];

    // first find in mlog
    const CHeader *header = static_cast<const CHeader*>(shard.find(key));
    if (header) {
      if (header->IsConnected) {
        callback(header + 1, header->Size);
        return true;
      } else {
        return false;
      }
    }

    // find in storage
    auto &storage = this->OnDiskStorage_[shardIndex];
    rocksdb::Slice keySlice(reinterpret_cast<const char*>(&key), sizeof(CKey));
    std::string value;
    if (storage->Get(rocksdb::ReadOptions(), keySlice, &value).ok()) {
      callback(value.data(), value.size());
      return true;
    }

    return false;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->Shards_[shardIndex];
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();
    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));

    xmstream serializedKeys;
    for (const auto &v: shard.AllKeysData) {
      if (v.second.empty())
        continue;

      const CHeader *header = static_cast<CHeader*>(v.second.back().KeyData);
      const void *data = header + 1;

      auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(&header->Key), sizeof(CKey));
      if (header->IsConnected) {
        auto valueSlice = rocksdb::Slice(static_cast<const char*>(data), header->Size);
        batch.Put(keySlice, valueSlice);
      } else {
        batch.Delete(keySlice);
      }
    }

    storage->Write(rocksdb::WriteOptions(), &batch);
    shard.reset();
  }
};

template<typename CKey>
class CBaseArrayFixed : public Base<CKey> {
private:
#pragma pack(push, 1)
  struct CHeader {
    CKey Key;
    size_t Size;
    int64_t Count;
    int64_t ChunkOffset;
  };


  struct CChunkKey {
    CKey Key;
    uint64_t Index;
  };
#pragma pack(pop)

private:
  class MergeOperator : public rocksdb::MergeOperator {
    virtual bool FullMerge(const rocksdb::Slice&,
                           const rocksdb::Slice *existing_value,
                           const std::deque<std::string> &operand_list,
                           std::string *new_value,
                           rocksdb::Logger*) const override {
      assert(existing_value);
      new_value->assign(existing_value->data(), existing_value->data() + existing_value->size());
      for (const auto &operand: operand_list) {
        assert(operand.size() >= 8);

        uint64_t offset = *reinterpret_cast<const uint64_t*>(operand.data());
        uint64_t requiredSize = offset + operand.size() - sizeof(uint64_t);
        if (new_value->size() < requiredSize)
          new_value->resize(requiredSize);

        memcpy(new_value->data() + offset, operand.data() + sizeof(uint64_t), operand.size() - sizeof(uint64_t));
      }
      return true;
    }

    virtual bool PartialMerge(const rocksdb::Slice&,
                              const rocksdb::Slice &left,
                              const rocksdb::Slice &right,
                              std::string *out,
                              rocksdb::Logger*) const override {
      assert(left.size() >= 8);
      assert(right.size() >= 8);
      uint64_t leftChunkOffset = *reinterpret_cast<const uint64_t*>(left.data());
      uint64_t rightChunkOffset = *reinterpret_cast<const uint64_t*>(right.data());
      assert(leftChunkOffset + left.size() - sizeof(uint64_t) == rightChunkOffset);

      out->assign(left.data(), left.data() + left.size());
      out->append(right.data() + sizeof(uint64_t), right.data() + right.size());
      return true;
    }

    virtual const char* Name() const override {
      return "CBaseArrayFixed";
    }
  };

public:
  CBaseArrayFixed(const std::string &name, size_t valueSize, size_t chunkSize) :
    Base<CKey>(name), ValueSize_(valueSize), ChunkSize_(chunkSize) {}
  virtual ~CBaseArrayFixed() {}

  rocksdb::MergeOperator *mergeOperator() final { return new MergeOperator(); }

  void add(const BC::Proto::BlockHashTy &blockId, const CKey &key, const void *data, size_t size, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->Shards_[hasher(key) % this->BaseCfg_.ShardsNum];

    // get current array size
    const void *prevData = nullptr;
    size_t prevSize = 0;
    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader) {
      prevData = oldHeader + 1;
      prevSize = oldHeader->Size;
      prevCount = oldHeader->Count;
    }

    // allocate new buffer
    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader) + prevSize + size));
    uint8_t *dst = reinterpret_cast<uint8_t*>(header + 1);
    header->Key = key;
    header->Size = prevSize + size;
    header->Count = prevCount + count;
    if (prevData)
      memcpy(dst, prevData, prevSize);
    memcpy(dst + prevSize, data, size);

    shard.update(blockId, key, header);
  }

  void remove(const BC::Proto::BlockHashTy &blockId, const CKey &key, size_t count) {
    std::hash<CKey> hasher;
    CLog<CKey> &shard = this->Shards_[hasher(key) % this->BaseCfg_.ShardsNum];

    // get current array size
    int64_t prevCount = 0;
    const CHeader *oldHeader = static_cast<const CHeader*>(shard.find(key));
    if (oldHeader) {
      prevCount = oldHeader->Count;
    }

    CHeader *header = static_cast<CHeader*>(shard.Log.alloc(sizeof(CHeader)));
    header->Key = key;
    header->Size = 0;
    header->Count = prevCount - count;

    shard.update(blockId, key, header);
  }

  bool query(const CKey &key, size_t from, size_t count, xmstream &result, size_t *totalTxCount) {
    std::hash<CKey> hasher;
    size_t shardIdx = hasher(key) % this->BaseCfg_.ShardsNum;
    CLog<CKey> &mlog = this->Shards_[shardIdx];
    rocksdb::DB *storage = this->OnDiskStorage_[shardIdx].get();

    if (count == 0)
      return false;
    size_t firstChunk = from / ChunkSize_;
    size_t lastChunk = (from + count - 1) / ChunkSize_;

    std::vector<std::string> readResult;

    {
      std::vector<CChunkKey> chunkKeys;
      std::vector<rocksdb::Slice> allKeySlices;

      // add metadata key
      allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&key), sizeof(CKey)));
      // add keys for all chunks
      for (size_t chunkId = firstChunk; chunkId <= lastChunk; chunkId++) {
        CChunkKey &k = chunkKeys.emplace_back();
        k.Key = key;
        k.Index = xhtobe<uint64_t>(chunkId);
      }
      for (const auto &k: chunkKeys)
        allKeySlices.push_back(rocksdb::Slice(reinterpret_cast<const char*>(&k), sizeof(CChunkKey)));

      auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &readResult);
      if (!metadataReadResult[0].ok())
        return false;
    }

    int64_t onDiskArraySize = *reinterpret_cast<const uint64_t*>(readResult[0].data());
    int64_t onDiskAvailable = std::min((int64_t)(from + count), onDiskArraySize) - from;
    int64_t inMemoryOffset = std::max((int64_t)from - onDiskArraySize, (int64_t)0);
    int64_t remaining = count;
    *totalTxCount = onDiskArraySize;

    // Read disk data
    if (onDiskAvailable > 0) {
      int64_t chunkOffset = from % ChunkSize_;
      for (size_t chunkId = firstChunk, index = 1; chunkId <= lastChunk; chunkId++, index++) {
        const char *chunkData = readResult[index].data();
        int64_t count = readResult[index].size() / ValueSize_;
        int64_t available = count - chunkOffset;
        if (available <= 0)
          break;
        int64_t needToRead = std::min(available, remaining);

        memcpy(result.reserve(needToRead * ValueSize_), chunkData + chunkOffset * ValueSize_, needToRead * ValueSize_);
        remaining -= needToRead;
        chunkOffset = 0;
      }
    }

    // Read tail from memory (inMemoryOffset, remaining)
    const CHeader *header = static_cast<const CHeader*>(mlog.find(key));
    if (header) {
      const uint8_t *memoryData = reinterpret_cast<const uint8_t*>(header + 1);
      int64_t available = header->Count - inMemoryOffset;
      int64_t needToRead = std::min(available, remaining);
      if (needToRead > 0)
        memcpy(result.reserve(needToRead * ValueSize_), memoryData + inMemoryOffset*ValueSize_, needToRead * ValueSize_);
      *totalTxCount += header->Count;
    }

    // TODO: consistency check using MLog::generation
    return true;
  }

  virtual void flushImpl(const BC::Proto::BlockHashTy &blockId, size_t shardIndex) final {
    CLog<CKey> &shard = this->Shards_[shardIndex];
    MLog &mlog = shard.Log;
    rocksdb::DB *storage = this->OnDiskStorage_[shardIndex].get();

    // Collect all keys
    std::vector<CKey> allKeys;
    std::vector<CHeader*> allData;
    std::vector<rocksdb::Slice> allKeySlices;
    std::vector<std::string> metadata;

    for (const auto &v: shard.AllKeysData) {
      if (v.second.empty())
        continue;
      allKeys.emplace_back(v.first);
      allData.push_back((CHeader*)v.second.back().KeyData);
    }

    // Make slices for metadata reading
    for (size_t i = 0; i < allKeys.size(); i++)
      allKeySlices.emplace_back((const char*)&allKeys[i], sizeof(CKey));

    // Read metadata
    auto metadataReadResult = storage->MultiGet(rocksdb::ReadOptions(), allKeySlices, &metadata);

    // Processing keys
    rocksdb::WriteBatch batch;

    batch.Put(rocksdb::Slice("stamp"), rocksdb::Slice(reinterpret_cast<const char*>(blockId.begin()), sizeof(BC::Proto::BlockHashTy)));
    for (size_t i = 0; i < allKeys.size(); i++) {
      CHeader *header = allData[i];
      int64_t currentArraySize = 0;
      if (metadataReadResult[i].ok() && metadata[i].size() == sizeof(uint64_t))
        currentArraySize = *(int64_t*)metadata[i].data();

      int64_t currentChunksNum = currentArraySize / ChunkSize_ + (currentArraySize % ChunkSize_ != 0);
      int64_t newArraySize = currentArraySize + header->Count;
      if (newArraySize < 0)
        newArraySize = 0;

      int64_t newChunksNum = newArraySize / ChunkSize_ + (newArraySize % ChunkSize_ != 0);

      // Update metadata
      {
        auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(&header->Key), sizeof(CKey));
        if (newArraySize) {
          int64_t *arraySize = mlog.alloc<int64_t>();
          *arraySize = newArraySize;
          batch.Put(keySlice, rocksdb::Slice((const char*)arraySize, sizeof(int64_t)));
        } else {
          batch.Delete(keySlice);
        }
      }

      if (newChunksNum < currentChunksNum) {
        // Delete empty chunks
        for (int64_t chunkIdx = newChunksNum; chunkIdx < currentChunksNum; chunkIdx++) {
          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = chunkIdx;
          auto slice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          batch.Delete(slice);
        }
      } else {
        size_t remaining = header->Count;
        int64_t offset = currentArraySize;
        const char *dataPtr = reinterpret_cast<const char*>(&header->ChunkOffset);

        if (currentArraySize % ChunkSize_) {
          // Merge last chunk
          size_t writeSize = std::min(remaining, ChunkSize_ - (size_t)(currentArraySize % ChunkSize_));

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<int64_t>(currentChunksNum - 1);
          header->ChunkOffset = ValueSize_ * (offset % ChunkSize_);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * ValueSize_ + sizeof(int64_t));
          batch.Merge(keySlice, valueSlice);

          dataPtr += writeSize * ValueSize_ + sizeof(int64_t);
          remaining -= writeSize;
          offset += writeSize;
        } else {
          dataPtr += sizeof(uint64_t);
        }

        for (int64_t chunkIdx = currentChunksNum; chunkIdx < newChunksNum; chunkIdx++) {
          size_t writeSize = std::min(remaining, ChunkSize_);
          assert(writeSize != 0);

          CChunkKey *chunkKey = mlog.alloc<CChunkKey>();
          chunkKey->Key = allKeys[i];
          chunkKey->Index = xhtobe<int64_t>(chunkIdx);
          auto keySlice = rocksdb::Slice(reinterpret_cast<const char*>(chunkKey), sizeof(CChunkKey));
          auto valueSlice = rocksdb::Slice(dataPtr, writeSize * ValueSize_);
          batch.Put(keySlice, valueSlice);

          dataPtr += writeSize * ValueSize_;
          remaining -= writeSize;
          offset += writeSize;
        }
      }
    }

    storage->Write(rocksdb::WriteOptions(), &batch);
    shard.reset();
  }

public:
  size_t ValueSize_ = 0;
  size_t ChunkSize_ = 0;
};

// Interfaces
enum EInterfaceTy {
  EIQueryTransaction = 0,
  EIQueryAddrHistory
};

struct CQueryTransactionResult {
  BC::Proto::Transaction Tx;
  BC::Proto::TxHashTy Block;
  uint32_t TxNum;
  bool Found = false;
  bool DataCorrupted = false;
};

struct CQueryAddrHistory {
  std::vector<BC::Proto::TxHashTy> Transactions;
  size_t TotalTxCount;
};

class ITransactionDb {
public:
  virtual bool queryTransaction(const BC::Proto::TxHashTy &txid,
                                BlockInMemoryIndex &blockIndex,
                                BlockDatabase &blockDb,
                                CQueryTransactionResult &result) = 0;

  virtual bool searchUnspentOutput(const BC::Proto::TxHashTy &tx,
                                   uint32_t outputIndex,
                                   BlockInMemoryIndex &blockIndex,
                                   BlockDatabase &blockDb,
                                   xmstream &result) = 0;
};

class IAddrHistoryDb {
public:
  virtual bool queryAddrTxid(const BC::Proto::AddressTy &address, size_t from, size_t count, CQueryAddrHistory &result) = 0;
};

}
}
