#pragma once

#include "BC/proto.h"

namespace BC {
namespace Script {
 
enum {
  OP_0 = 0,
  OP_DUP = 0x76,
  OP_EQUALVERIFY = 0x88,
  OP_HASH160 = 0xA9,
  OP_CHECKSIG = 0xAC
};

bool decodeStandardOutput(const BC::Proto::TxOut &out, BC::Proto::AddressTy &address);
    
}
}
