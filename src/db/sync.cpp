#include "sync.h"
#include "archive.h"
#include "storage.h"
#include <chrono>
#include <deque>
#include <thread>

namespace BC {
namespace DB {

bool dbDisconnectBlocks(BC::DB::BaseInterface &db,
                        BlockInMemoryIndex &blockIndex,
                        BC::Common::ChainParams &chainParams,
                        BC::DB::Storage &storage,
                        std::vector<BC::Common::BlockIndex*> &forDisconnect)
{
  for (BC::Common::BlockIndex *index: forDisconnect) {
    // attach cannot refuse: hold the walk while the engine is over its
    // admission limit, the flusher drains it on its own
    while (db.pipelineFull())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto object = objectByIndex(index, chainParams, storage.blockDb());
    if (!object.get())
      return false;
    db.disconnect(index, *object.get()->block(), object.get()->linkedOutputs(), object.get()->validationDataConst(), blockIndex, storage.blockDb());
  }

  return true;
}

// A database wakes up at its own height; heights inside a batch are contiguous
static size_t tailFrom(uint32_t connectHeight, uint32_t firstHeight)
{
  return connectHeight > firstHeight ? connectHeight - firstHeight : 0;
}

// A batch the databases are still reading: submit() does not wait, so the refs have to outlive
// the call that posted it. The blocks they point at do not - those go with the last share
struct CInFlightBatch {
  std::vector<BC::DB::CBlockRef> Refs;
  BC::DB::Archive::CConnectTask Task;
};

bool dbConnectBlocks(BC::DB::UTXODb &utxoDb,
                     BC::Common::BlockIndex *utxoFirstBlock,
                     const std::vector<BC::Common::BlockIndex*> &archiveFirstBlocks,
                     BC::DB::Archive *archive,
                     BlockInMemoryIndex &blockIndex,
                     BC::Common::ChainParams &chainParams,
                     BC::DB::Storage &storage,
                     CBlockPipeline &pipeline,
                     const CBlockPipeline::CParams &params,
                     const char *name)
{
  const uint32_t utxoFirstHeight = utxoFirstBlock ?
                                   utxoFirstBlock->Height :
                                   std::numeric_limits<uint32_t>::max();

  BC::Common::BlockIndex *firstNeeded = utxoFirstBlock;
  uint32_t firstNeededHeight = utxoFirstHeight;

  for (BC::Common::BlockIndex *first: archiveFirstBlocks) {
    if (first && first->Height < firstNeededHeight) {
      firstNeeded = first;
      firstNeededHeight = first->Height;
    }
  }

  // A database opened empty asks to start at the genesis block, which is in no block file and
  // which no path ever connects - the chain starts above it
  if (firstNeeded == blockIndex.genesis())
    firstNeeded = firstNeeded->Next;

  if (!firstNeeded) {
    LOG_F(INFO, "%s is up to date", name);
    return true;
  }

  BC::Common::BlockIndex *best = blockIndex.best();
  const uint32_t count = best->Height - firstNeeded->Height + 1;
  LOG_F(INFO, "Update %s: connecting %u blocks", name, count);

  // As deep as the pipeline admits work: inside this window a fast database is free to run
  // ahead of a slow one
  const size_t ringDepth = params.ReadyQueueDepth + params.PrepLanes;
  std::deque<std::unique_ptr<CInFlightBatch>> ring;

  uint32_t fed = 0;
  // Portions are counted in blocks, and a block near the tip is a hundred times
  // the one at the genesis - hence the bytes, the only comparable measure
  uint64_t fedBytes = 0;
  unsigned portionNum = 0;
  unsigned portionSize = count / 20 + 1;

  // Called by the serial stage, in chain order, one batch at a time
  pipeline.setCatchUpSink([&](std::unique_ptr<CSegment> segment) {
    auto entry = std::make_unique<CInFlightBatch>();
    entry->Refs.reserve(segment->Objects.size());
    for (CSegment::CObject &object: segment->Objects) {
      BC::Common::CIndexCacheObject *cached = object.Object.get();
      entry->Refs.push_back(BC::DB::CBlockRef{object.Index, cached->block(), &cached->linkedOutputs(), &cached->validationData()});
    }

    const uint32_t firstHeight = entry->Refs.front().Index->Height;

    // This thread reads the batch below, so it takes a share of its own; submit() takes one more
    // for every database
    CSegment *data = segment.release();
    data->shareAdd(1);

    // Archive first and without waiting: its workers chew while this thread takes the utxo and
    // the lanes prepare the next batch
    if (archive) {
      entry->Task.Batch = entry->Refs;
      entry->Task.BlockIndex = &blockIndex;
      entry->Task.BlockDb = &storage.blockDb();
      entry->Task.Segment = data;
      entry->Task.FirstHeight = firstHeight;
      archive->submit(entry->Task);
    }

    // utxo has no worker of its own: it belongs to this thread
    const size_t utxoSkip = tailFrom(utxoFirstHeight, firstHeight);
    if (utxoSkip < entry->Refs.size())
      utxoDb.connect(BC::DB::CBlockBatch(entry->Refs).subspan(utxoSkip), blockIndex, storage.blockDb());

    fed += static_cast<uint32_t>(entry->Refs.size());
    fedBytes += data->Size;
    while (portionNum < 20 && fed >= (portionNum + 1) * portionSize) {
      portionNum++;
      LOG_F(INFO, "%u%% done, block %u, %.1lf MB", portionNum*5, entry->Refs.back().Index->Height, fedBytes / 1048576.0);
    }

    // This thread is done with the blocks; whichever database finishes last frees them
    CSegment::shareRelease(data);

    // The ring is how far the feed may run ahead of the slowest worker. It holds the refs the
    // workers still read, not the blocks - those are gone by then
    if (archive) {
      ring.push_back(std::move(entry));
      while (ring.size() > ringDepth) {
        archive->wait(ring.front()->Task);
        ring.pop_front();
      }
    }
  });

  auto startTime = std::chrono::steady_clock::now();
  uint64_t totalBytesRead = 0;

  CCatchUpReader reader(storage.blockDb(),
                        chainParams,
                        storage.cache(),
                        firstNeeded,
                        params.SegmentSizeLimit,
                        params.SegmentBlocksLimit,
                        params.WaveThreads);
  for (;;) {
    // The same limits the reindex reader obeys: the utxo engine over its admission limit, and
    // decoded blocks the chain has not taken yet
    while (pipeline.throttled())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::unique_ptr<CSegment> segment = reader.next();
    if (!segment)
      break;

    totalBytesRead += segment->Size;
    // Returns once the pipeline has room for it
    pipeline.feed(std::move(segment));
  }

  pipeline.waitDrained();

  // The pipeline is empty, the databases may still be reading the last batches of it
  while (!ring.empty()) {
    archive->wait(ring.front()->Task);
    ring.pop_front();
  }
  pipeline.setCatchUpSink(nullptr);

  if (reader.failed())
    return false;

  LOG_F(INFO, "100%% done");

  // Before the compaction wait, so the rows it writes are part of the same
  // debt - and inside the measure, because the archive is not caught up until
  // what the catch-up put off has been built
  if (archive && !archive->finishInitialBuild())
    return false;

  // The databases still owe the backend a compaction here, and until it is paid
  // the speed below counts megabytes nobody has finished writing
  if (archive && archive->compactAfterSync()) {
    auto compactStart = std::chrono::steady_clock::now();
    std::thread utxoWorker([&utxoDb]() { utxoDb.flush(); utxoDb.settle(); });
    archive->settle();
    utxoWorker.join();
    LOG_F(INFO, "Update %s: compaction settled in %.1lf seconds", name,
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - compactStart).count() / 1000.0);
  }

  // Same measure the reindex prints: block bytes over the wall time, final waits included
  double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count() / 1000.0;
  double megabytes = totalBytesRead / 1048576.0;
  LOG_F(INFO,
        "Update %s speed: %.2lf MB/s (%.1lf MB in %.1lf seconds)",
        name,
        elapsed > 0.0 ? megabytes / elapsed : 0.0,
        megabytes,
        elapsed);
  return true;
}

}
}
