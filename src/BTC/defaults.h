#pragma once

#include <stddef.h>
#include <stdint.h>

namespace BTC {
namespace Common {

static constexpr size_t MaxBlockSize = 1000000;
static constexpr uint32_t BlocksFileLimit = 128*1048576;
static constexpr size_t DefaultBlockCacheSize = 512*1048576;

}
}
