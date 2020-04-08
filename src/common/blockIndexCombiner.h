// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/bc.h"
#include "combiner.h"

class BlockProcessingTask : public BC::Common::BlockIndex {
public:
  BlockProcessingTask *next() { return static_cast<BlockProcessingTask*>(CombinerNext); }
  void setNext(BlockProcessingTask *task) { CombinerNext = task; }
  void release() {}
};
