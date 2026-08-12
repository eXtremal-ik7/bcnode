// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The connect contract: what a database is to the chain, and in what shape the
// chain hands blocks to it. What the databases answer on the read side is a
// separate menu - queries.h - and nothing here knows about it

#include "common/blockDataBase.h"

#include "config4cpp/Configuration.h"
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace BC {
namespace DB {

// One block as a database sees it: the index and the three parsed pieces the
// walk reads. Pointers, because these live in an array. The index is not const
// only because the storage queue writes back where the block landed on disk -
// a database reads it and nothing more
struct CBlockRef {
  BC::Common::BlockIndex *Index = nullptr;
  const BC::Proto::Block *Block = nullptr;
  const BC::Proto::CBlockLinkedOutputs *LinkedOutputs = nullptr;
  const BC::Proto::CBlockValidationData *ValidationData = nullptr;
};

// The unit of a connect: blocks in chain order, applied as one operation, so
// the database ends up at the last of them. A block connected on its own is a
// batch of one. A disconnect has no such unit and takes a single block: it
// walks down a fork of unknown depth, a block at a time, off the hot path
using CBlockBatch = std::span<const CBlockRef>;

// What a database is to the chain: open it, tell it where the chain went, ask
// it to write. How it holds the window in between - published views, see
// kvbase.h - is none of the caller's business
class BaseInterface {
public:
  virtual ~BaseInterface() {}

  virtual bool initialize(BlockInMemoryIndex &blockIndex,
                          const std::filesystem::path &dbPath,
                          config4cpp::Configuration *cfg,
                          BC::Common::BlockIndex **forConnect,
                          std::vector<BC::Common::BlockIndex*> &forDisconnect) = 0;

  virtual void connect(CBlockBatch batch,
                       BlockInMemoryIndex &blockIndex,
                       BlockDatabase &blockDb) = 0;

  virtual void disconnect(const BC::Common::BlockIndex *index,
                          const BC::Proto::Block &block,
                          const BC::Proto::CBlockLinkedOutputs &linkedOutputs,
                          const BC::Proto::CBlockValidationData &validationData,
                          BlockInMemoryIndex &blockIndex,
                          BlockDatabase &blockDb) = 0;

  virtual void *interface(int interface) = 0;

  // Windows attached but not flushed yet, over the admission limit: attach
  // cannot refuse, so whoever feeds connects must ask this before starting work
  virtual bool pipelineFull() const { return false; }

  virtual const std::string &name() const = 0;

  virtual void flush() = 0;

  // Second half of initialize, run once the startup catch-up is over: what a
  // database is better off building from the finished data than maintaining
  // block by block goes here
  virtual bool finishInitialBuild() = 0;

  // Wait out the work the writes left owing: until the backend has worked it
  // off, a speed measured over them counts bytes nobody finished writing
  virtual void settle() = 0;
};

// Where a database keyed by txid starts its walk over a block. A BIP30 repeat
// brings a coinbase already stored under that key - the same transaction byte
// for byte - so rewriting it would only move the record to the newer block, and
// the remove that undoes it would take the twin's record with it while the twin
// is still connected. The chain params carry both inclusions for the query side
static inline size_t firstTx(const BC::Proto::CBlockValidationData &validationData) {
  return validationData.CoinbaseRepeat ? 1 : 0;
}

}
}
