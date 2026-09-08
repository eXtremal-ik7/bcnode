#include "proto.h"

#include "crypto/sha3.h"

namespace ETH {

CHash256 blockHash(const uint8_t *headerRlp, size_t size)
{
  CHash256 hash;
  keccak(headerRlp, size, hash.begin(), hash.size());
  return hash;
}

}
