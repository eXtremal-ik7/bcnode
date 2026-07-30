// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockBatch.h"
#include "blockDataBase.h"
#include "db/storage.h"
#include "loguru.hpp"

#include <algorithm>

thread_local CStagedChunk *CBlockAssembler::FileChunk_ = nullptr;

bool CBatchExecutor::start(unsigned threadsNum, CBlockProc proc)
{
  Proc_ = proc;
  Stopped_ = false;
  Queue_.init(QueueCapacity);
  for (unsigned i = 0; i < threadsNum; i++)
    Threads_.emplace_back([this]() { worker(); });
  return true;
}

void CBatchExecutor::stop()
{
  Stopped_.store(true);
  // One ticket per worker: whatever was submitted is still drained first
  Tickets_.release(static_cast<std::ptrdiff_t>(Threads_.size()));
  for (auto &thread: Threads_)
    thread.join();
  Threads_.clear();
}

void CBatchExecutor::submit(std::unique_ptr<CBlockBatch> &&batch)
{
  InFlight_.fetch_add(1, std::memory_order_relaxed);

  CBlockBatch *raw = batch.release();
  // Never taken: the ring holds far more batches than the staging limit allows in flight
  while (!Queue_.push(raw))
    std::this_thread::yield();

  Tickets_.release();
}

void CBatchExecutor::waitIdle()
{
  size_t inFlight;
  while ((inFlight = InFlight_.load(std::memory_order_acquire)) != 0)
    InFlight_.wait(inFlight, std::memory_order_acquire);
}

void CBatchExecutor::worker()
{
  // No InitializeWorkerThread() here: those ids index arrays sized by the number of asyncio
  // threads (Peer::SendStreams, BlockSource::LastDequeued_)
  loguru::set_thread_name("batch");

  // Reused by all blocks of this thread
  BC::Common::CheckConsensusCtx ccCtx;
  BC::Common::checkConsensusInitialize(ccCtx);

  for (;;) {
    Tickets_.acquire();

    CBlockBatch *batch = Queue_.pop();
    while (!batch) {
      // A stopped executor still drains what was submitted; an empty ring means this ticket
      // came from stop()
      if (Stopped_.load(std::memory_order_relaxed) && Queue_.empty())
        return;
      // A push has claimed its cell but has not published it yet
      std::this_thread::yield();
      batch = Queue_.pop();
    }

    for (auto &block: batch->Blocks)
      Proc_(block, ccCtx);

    // Releases the block data, and the block file buffer with the last batch using it
    delete batch;

    if (InFlight_.fetch_sub(1, std::memory_order_acq_rel) == 1)
      InFlight_.notify_all();
  }
}


bool CBlockAssembler::start(BlockInMemoryIndex &blockIndex,
                            BC::Common::ChainParams &chainParams,
                            BC::DB::Storage &storage,
                            unsigned threadsNum,
                            const CParams &params)
{
  BlockIndex_ = &blockIndex;
  ChainParams_ = &chainParams;
  Storage_ = &storage;
  Params_ = params;
  // A throttled source must still be able to fill several batches, else it would wait on a
  // batch that is never cut
  Params_.StagingSizeLimit = std::max(Params_.StagingSizeLimit, Params_.BatchSizeLimit * 4);
  Frontier_ = blockIndex.best();
  LastCutTime_ = std::chrono::steady_clock::now();
  LastStatTime_ = LastCutTime_;

  LOG_F(INFO,
        "Batch pipeline: %u workers, batch %zu blocks / %.1lfMb, staging %.1lfMb",
        threadsNum,
        Params_.BatchBlocksLimit,
        Params_.BatchSizeLimit / 1048576.0,
        Params_.StagingSizeLimit / 1048576.0);

  Started_ = Executor_.start(threadsNum, [this](CStagedBlock &block, BC::Common::CheckConsensusCtx &ccCtx) { process(block, ccCtx); });
  return Started_;
}

void CBlockAssembler::stop()
{
  if (!Started_)
    return;
  flushAll();
  Executor_.stop();
  Started_ = false;
}

CBlockAssembler::EResult CBlockAssembler::attachFromFile(const intrusive_ptr<CRawBlockData> &buffer,
                                                         uint32_t offset,
                                                         uint32_t size,
                                                         uint32_t fileNo,
                                                         uint32_t fileOffset)
{
  // Header is all we parse here: it names the index the data is attached to
  BC::Proto::BlockHeader header;
  {
    xmstream stream(static_cast<uint8_t*>(buffer.get()->data()) + offset, size);
    if (!BC::unserializeAndCheck(stream, header))
      return Invalid;
  }

  if (!FileChunk_) {
    FileChunk_ = new CStagedChunk;
    FileChunk_->Blocks.reserve(Params_.FileChunkBlocks);
  }

  FileChunk_->Blocks.emplace_back();
  CStagedBlock &block = FileChunk_->Blocks.back();
  block.Buffer = buffer;
  block.Offset = offset;
  block.Size = size;
  block.FileNo = fileNo;
  block.FileOffset = fileOffset;
  // No index reserved (block we already have) -> full AddBlock path in the worker
  block.Index = attachBlockData(*BlockIndex_, *ChainParams_, header, header.GetHash(), &block.CheckWork);

  if (FileChunk_->Blocks.size() >= Params_.FileChunkBlocks)
    publishFileChunk();

  return Staged;
}

CBlockAssembler::EResult CBlockAssembler::attachFromNetwork(void *data,
                                                            uint32_t size,
                                                            uint32_t memorySize,
                                                            const BC::Proto::BlockHeader &header,
                                                            const BC::Proto::BlockHashTy &hash,
                                                            BC::Common::BlockIndex **staged)
{
  bool checkWork = true;
  BC::Common::BlockIndex *index = attachBlockData(*BlockIndex_, *ChainParams_, header, hash, &checkWork);
  // Already have this block, or another thread is attaching the same data
  if (!index)
    return NotStaged;
  *staged = index;

  // Published right away: at network rates latency matters more than atomic traffic, and a
  // chunk left in a peer thread would be a hole no other thread can close
  CStagedChunk *chunk = new CStagedChunk;
  chunk->Blocks.emplace_back();
  CStagedBlock &block = chunk->Blocks.back();
  block.Index = index;
  block.Size = size;
  block.CheckWork = checkWork;
  block.Exclusive = true;
  // Accounted in the block cache: the getdata scheduler throttles on it
  block.Buffer = intrusive_ptr<CRawBlockData>(new CRawBlockData(data, memorySize, &Storage_->cache()));

  publish(chunk);
  return Staged;
}

void CBlockAssembler::publish(CStagedChunk *chunk)
{
  Inbox_.push(chunk);
  drain();
}

void CBlockAssembler::publishFileChunk()
{
  if (!FileChunk_)
    return;

  CStagedChunk *chunk = FileChunk_;
  FileChunk_ = nullptr;
  publish(chunk);
}

// Sequencer: runs the state machine over everything in the inbox, then gives the role back.
// A producer that could not take the role has published its chunk already, so the recheck on
// release picks it up
void CBlockAssembler::drain()
{
  if (!Inbox_.tryAcquire())
    return;

  CReadyBatches ready;
  for (;;) {
    serveRequests(ready);

    CStagedChunk *chunk = Inbox_.take();
    while (chunk) {
      CStagedChunk *next = chunk->Next;
      for (auto &block: chunk->Blocks)
        stage(std::move(block), ready);
      delete chunk;
      chunk = next;

      // Workers must not wait for the whole drain
      submit(ready);
    }

    submit(ready);
    if (!Inbox_.release([this]() { return requestsPending(); }))
      return;
  }
}

bool CBlockAssembler::requestsPending() const
{
  return CutAllReq_.load() != CutAllServed_.load() ||
         CutReq_.load() != CutServed_.load() ||
         TimeoutReq_.load();
}

void CBlockAssembler::serveRequests(CReadyBatches &ready)
{
  uint64_t cutAll = CutAllReq_.load();
  uint64_t cutOne = CutReq_.load();
  if (cutAll != CutAllServed_.load(std::memory_order_relaxed)) {
    cut(ready);
    cutStaged(ready);
    // The waiter must not wake up before its batches are in flight
    submit(ready);
    // A plain cut is subsumed by this one
    CutServed_.store(cutOne, std::memory_order_relaxed);
    CutAllServed_.store(cutAll);
    CutAllServed_.notify_all();
  } else if (cutOne != CutServed_.load(std::memory_order_relaxed)) {
    cut(ready);
    submit(ready);
    CutServed_.store(cutOne, std::memory_order_relaxed);
  }

  if (TimeoutReq_.exchange(false))
    cutOnTimeout(ready);

  reportStats();
}

void CBlockAssembler::cutOnTimeout(CReadyBatches &ready)
{
  bool haveWork = (Batch_ && !Batch_->Blocks.empty()) || (Unplaced_ && !Unplaced_->Blocks.empty()) || !Staged_.empty();
  if (!haveWork)
    return;
  if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - LastCutTime_).count() < Params_.FlushTimeoutMs)
    return;

  cut(ready);
  // Nothing moved for a while: a block lost on the way leaves a hole that never closes, and
  // blocks kept out of the pipeline are not asked from peers again
  size_t held = Staged_.size();
  cutStaged(ready);
  if (held)
    LOG_F(INFO, "Batch pipeline: %zu blocks released on timeout (predecessor never arrived)", held);
}

void CBlockAssembler::stage(CStagedBlock &&block, CReadyBatches &ready)
{
  StagedBytes_ += block.Size;

  if (!block.Index) {
    addUnplaced(std::move(block), ready);
    return;
  }

  BC::Common::BlockIndex *prev = block.Index->Prev;
  if (prev == Frontier_) {
    // Continues the run: straight into the batch, no round trip through Staged_
    extendRun(std::move(block), ready);
    advance(ready);
    return;
  }

  bool extend = true;
  if (!prev->Batched && !prev->OnChain) {
    // Predecessor's data may still arrive: arrival order inside a block file is not exact,
    // and a hole in the network stream is filled later
  } else if (block.Index->Height > Frontier_->Height) {
    // Ahead of the run but not continuing it (frontier left behind, or a break): close the
    // run and start a new one here
    cut(ready);
    Frontier_ = prev;
  } else {
    // Behind the run: fork of a batched block, or a block seen twice
    extend = false;
  }

  // try_emplace, not emplace: a failed emplace would have moved the block away. It fails when
  // two blocks wait for the same predecessor, and only one of them can extend the run
  size_t size = block.Size;
  if (extend && Staged_.try_emplace(prev, std::move(block)).second) {
    HoldBytes_ += size;
    advance(ready);
  } else {
    addUnplaced(std::move(block), ready);
  }
}

void CBlockAssembler::extendRun(CStagedBlock &&block, CReadyBatches &ready)
{
  if (!Batch_)
    Batch_.reset(new CBlockBatch);

  Frontier_ = block.Index;
  Frontier_->Batched = true;
  Batch_->Bytes += block.Size;
  Batch_->Blocks.push_back(std::move(block));

  if (Batch_->Blocks.size() >= Params_.BatchBlocksLimit || Batch_->Bytes >= Params_.BatchSizeLimit)
    cut(ready);
}

void CBlockAssembler::advance(CReadyBatches &ready)
{
  for (;;) {
    auto It = Staged_.find(Frontier_);
    if (It == Staged_.end())
      break;

    HoldBytes_ -= It->second.Size;
    CStagedBlock block = std::move(It->second);
    Staged_.erase(It);
    extendRun(std::move(block), ready);
  }

  // Too much waiting behind a hole: hand it over unordered rather than keep the workers idle
  if (Staged_.size() >= Params_.HoldBlocksLimit || HoldBytes_ >= Params_.HoldSizeLimit) {
    cut(ready);
    cutStaged(ready);
  }
}

void CBlockAssembler::addUnplaced(CStagedBlock &&block, CReadyBatches &ready)
{
  if (!Unplaced_) {
    Unplaced_.reset(new CBlockBatch);
    Unplaced_->Continuous = false;
  }

  if (block.Index)
    block.Index->Batched = true;
  Unplaced_->Bytes += block.Size;
  Unplaced_->Blocks.push_back(std::move(block));
  if (Unplaced_->Blocks.size() >= Params_.BatchBlocksLimit || Unplaced_->Bytes >= Params_.BatchSizeLimit) {
    BatchCount_.fetch_add(1, std::memory_order_relaxed);
    ready.push_back(std::move(Unplaced_));
  }
}

void CBlockAssembler::cut(CReadyBatches &ready)
{
  if (Batch_) {
    if (!Batch_->Blocks.empty()) {
      BatchCount_.fetch_add(1, std::memory_order_relaxed);
      ready.push_back(std::move(Batch_));
    }
    Batch_.reset();
  }

  if (Unplaced_) {
    if (!Unplaced_->Blocks.empty()) {
      BatchCount_.fetch_add(1, std::memory_order_relaxed);
      ready.push_back(std::move(Unplaced_));
    }
    Unplaced_.reset();
  }

  LastCutTime_ = std::chrono::steady_clock::now();
}

void CBlockAssembler::cutStaged(CReadyBatches &ready)
{
  if (Staged_.empty())
    return;

  std::vector<CStagedBlock> blocks;
  blocks.reserve(Staged_.size());
  for (auto &It: Staged_) {
    It.second.Index->Batched = true;
    blocks.push_back(std::move(It.second));
  }

  Staged_.clear();
  HoldBytes_ = 0;

  // Height order gives the serial stage the longest run it can get
  std::sort(blocks.begin(), blocks.end(), [](const CStagedBlock &l, const CStagedBlock &r) {
    return l.Index->Height < r.Index->Height;
  });

  // Split as usual: one batch is one worker, a big one would be a serial stretch
  std::unique_ptr<CBlockBatch> batch;
  for (auto &block: blocks) {
    if (!batch) {
      batch.reset(new CBlockBatch);
      batch->Continuous = false;
    }

    batch->Bytes += block.Size;
    batch->Blocks.push_back(std::move(block));
    if (batch->Blocks.size() >= Params_.BatchBlocksLimit || batch->Bytes >= Params_.BatchSizeLimit) {
      BatchCount_.fetch_add(1, std::memory_order_relaxed);
      ready.push_back(std::move(batch));
    }
  }

  if (batch) {
    BatchCount_.fetch_add(1, std::memory_order_relaxed);
    ready.push_back(std::move(batch));
  }

  OutOfOrderBlocks_.fetch_add(blocks.size(), std::memory_order_relaxed);
}

void CBlockAssembler::submit(CReadyBatches &ready)
{
  for (auto &batch: ready)
    Executor_.submit(std::move(batch));
  ready.clear();
}

void CBlockAssembler::flush()
{
  publishFileChunk();
  CutReq_.fetch_add(1);
  drain();
}

void CBlockAssembler::flushAll()
{
  publishFileChunk();

  uint64_t ticket = CutAllReq_.fetch_add(1) + 1;
  drain();

  // Another thread holds the role: it serves the request on its way out
  uint64_t served = CutAllServed_.load();
  while (served < ticket) {
    CutAllServed_.wait(served);
    served = CutAllServed_.load();
  }
}

void CBlockAssembler::flushOnTimeout()
{
  TimeoutReq_.store(true);
  drain();
}

void CBlockAssembler::reportStats()
{
  constexpr unsigned intervalSeconds = 30;
  uint64_t batchCount = BatchCount_.load(std::memory_order_relaxed);
  auto now = std::chrono::steady_clock::now();
  if (batchCount == LastStatBatchCount_ ||
      std::chrono::duration_cast<std::chrono::seconds>(now - LastStatTime_).count() < intervalSeconds)
    return;

  LOG_F(INFO, "Batch pipeline: %zu batches, %zu blocks out of order, staging %.1lfMb, %zu blocks held",
        static_cast<size_t>(batchCount),
        static_cast<size_t>(OutOfOrderBlocks_.load(std::memory_order_relaxed)),
        StagedBytes_.load(std::memory_order_relaxed) / 1048576.0,
        Staged_.size());

  LastStatTime_ = now;
  LastStatBatchCount_ = batchCount;
}

void CBlockAssembler::waitIdle()
{
  Executor_.waitIdle();
}

void CBlockAssembler::process(CStagedBlock &block, BC::Common::CheckConsensusCtx &ccCtx)
{
  if (!processBlockData(*BlockIndex_, *ChainParams_, *Storage_, block, ccCtx, nullptr)) {
    // From a block file it is a load error and stops the reindex; from the network just a bad block
    if (block.FileNo != std::numeric_limits<uint32_t>::max())
      Failed_ = true;
  }

  StagedBytes_ -= block.Size;
}
