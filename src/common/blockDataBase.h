// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include <filesystem>
#include <functional>
#include <thread>

struct BCNodeContext;

struct StoreTask {
  uint64_t id;
  BC::Common::BlockIndex *index;
};

typedef std::function<void(const std::vector<BC::Common::BlockIndex*>&)> newBestCallback;

BC::Common::BlockIndex *AddHeader(BCNodeContext &context,
                                  const BC::Proto::BlockHeader &header,
                                  BC::Common::CheckConsensusCtx &ccCtx);

BC::Common::BlockIndex *AddBlock(BCNodeContext &context,
                                 SerializedDataObject *serialized,
                                 BC::Common::CheckConsensusCtx &ccCtx,
                                 newBestCallback callback,
                                 uint32_t fileNo=std::numeric_limits<uint32_t>::max(),
                                 uint32_t fileOffset=std::numeric_limits<uint32_t>::max());

bool loadingBlockIndex(BCNodeContext &context);
bool reindex(BCNodeContext &config);

bool initializeStorage(BCNodeContext &context, std::thread &storageThread);
void shutdownStorage(BCNodeContext &context);

class BlockSearcher {
private:
  static constexpr unsigned MaxIoSize = 4*1048576;

private:
  BCNodeContext *context = nullptr;
  uint32_t fileNo = std::numeric_limits<uint32_t>::max();
  uint32_t fileOffsetBegin = std::numeric_limits<uint32_t>::max();
  uint32_t fileOffsetCurrent = std::numeric_limits<uint32_t>::max();

  xmstream stream;
  std::filesystem::path blocksDirectory;

public:
  BlockSearcher(BCNodeContext &context);

  BC::Common::BlockIndex *add(const BC::Proto::BlockHashTy &hash);
  void *next(size_t *size);
  void fetchPending();
};
