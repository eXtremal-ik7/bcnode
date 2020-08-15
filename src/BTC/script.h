#pragma once

#include "BC/proto.h"

namespace BTC {
class Script {
public:
  enum {
    OP_0 = 0,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };

  static bool decodeStandardOutput(const BC::Proto::TxOut &out, BC::Proto::AddressTy &address);
    
};

}
