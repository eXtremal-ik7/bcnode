#pragma once

#include "BC/proto.h"

namespace BTC {
class Script {
public:
  enum {
    OP_0 = 0,
    OP_PUSH_1 = 0x01,
    OP_PUSH20 = 0x14,
    OP_PUSH_33 = 0x21,
    OP_PUSH_65 = 0x41,
    OP_PUSH_75 = 0x4B,
    OP_PUSHDATA1 = 0x4C,
    OP_PUSHDATA2 = 0x4D,
    OP_PUSHDATA4 = 0x4E,
    OP_1NEGATE = 0x4F,
    OP_1 = 0x51,
    OP_2 = 0x52,
    OP_3 = 0x53,
    OP_4 = 0x54,
    OP_5 = 0x55,
    OP_6 = 0x56,
    OP_7 = 0x57,
    OP_8 = 0x58,
    OP_9 = 0x59,
    OP_10 = 0x5A,
    OP_11 = 0x5B,
    OP_12 = 0x5C,
    OP_13 = 0x5D,
    OP_14 = 0x5E,
    OP_15 = 0x5F,
    OP_16 = 0x60,
    OP_NOP = 0x61,
    OP_RETURN = 0x6A,
    OP_DUP = 0x76,
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,
    OP_HASH160 = 0xA9,
    OP_CHECKSIG = 0xAC
  };

#pragma pack(push, 1)
  struct UnspentOutputInfo {
    enum EType {
      ENonStandard = 0,
      EOpReturn,
      EPubKey,
      EPubKeyHash,
      EScriptHash,
      EInvalid
    };

    uint8_t Type;
    uint8_t IsLocalTx;
    uint8_t IsPubKeyCompressed;

    int64_t Value;

    union {
      uint8_t PubKeyCompressed[33];
      BaseBlob<160> PubKeyHash;
      BaseBlob<160> ScriptHash;
      uint8_t CustomData[1];
    };

    static size_t customDataOffset() { return offsetof(UnspentOutputInfo, CustomData); }
  };
#pragma pack(pop)

  static std::string addressToBase58(UnspentOutputInfo::EType type,
                                     BC::Proto::AddressTy &address,
                                     const std::vector<uint8_t> &pubkeyPrefix,
                                     const std::vector<uint8_t> &scriptPrefix);

  static UnspentOutputInfo::EType extractSingleAddress(const BC::Proto::TxOut &txOut, BC::Proto::AddressTy &address);
  static bool extractSingleAddress(const UnspentOutputInfo &info, BC::Proto::AddressTy &address);

  static void parseTransactionOutput(const BC::Proto::TxOut &out, xmstream &unspentOutputInfo);
    
};

}
