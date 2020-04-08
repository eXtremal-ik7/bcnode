#pragma once

#include "defaults.h"
#include "proto.h"

namespace BTC {
namespace Common {

template<typename T>
static inline bool checkBlockSize(const T &block, size_t serializedSize) {
   // TODO: segwit check
   return !(block.vtx.empty() || block.vtx.size() > MaxBlockSize || serializedSize > MaxBlockSize);
}

}
}
