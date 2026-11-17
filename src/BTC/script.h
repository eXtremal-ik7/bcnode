#pragma once

#include "BC/proto.h"

namespace BTC {
class Script {
public:
  // Core's opcodetype
  enum EOpcode {
    OP_0 = 0,
    OP_PUSH_1 = 0x01,
    OP_PUSH20 = 0x14,
    OP_PUSH32 = 0x20,
    OP_PUSH_33 = 0x21,
    OP_PUSH_65 = 0x41,
    OP_PUSH_75 = 0x4B,
    OP_PUSHDATA1 = 0x4C,
    OP_PUSHDATA2 = 0x4D,
    OP_PUSHDATA4 = 0x4E,
    OP_1NEGATE = 0x4F,
    OP_RESERVED = 0x50,
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
    // Control
    OP_NOP = 0x61,
    OP_VER = 0x62,
    OP_IF = 0x63,
    OP_NOTIF = 0x64,
    OP_VERIF = 0x65,
    OP_VERNOTIF = 0x66,
    OP_ELSE = 0x67,
    OP_ENDIF = 0x68,
    OP_VERIFY = 0x69,
    OP_RETURN = 0x6A,
    // Stack
    OP_TOALTSTACK = 0x6B,
    OP_FROMALTSTACK = 0x6C,
    OP_2DROP = 0x6D,
    OP_2DUP = 0x6E,
    OP_3DUP = 0x6F,
    OP_2OVER = 0x70,
    OP_2ROT = 0x71,
    OP_2SWAP = 0x72,
    OP_IFDUP = 0x73,
    OP_DEPTH = 0x74,
    OP_DROP = 0x75,
    OP_DUP = 0x76,
    OP_NIP = 0x77,
    OP_OVER = 0x78,
    OP_PICK = 0x79,
    OP_ROLL = 0x7A,
    OP_ROT = 0x7B,
    OP_SWAP = 0x7C,
    OP_TUCK = 0x7D,
    // Splice
    OP_CAT = 0x7E,
    OP_SUBSTR = 0x7F,
    OP_LEFT = 0x80,
    OP_RIGHT = 0x81,
    OP_SIZE = 0x82,
    // Bitwise logic
    OP_INVERT = 0x83,
    OP_AND = 0x84,
    OP_OR = 0x85,
    OP_XOR = 0x86,
    OP_EQUAL = 0x87,
    OP_EQUALVERIFY = 0x88,
    OP_RESERVED1 = 0x89,
    OP_RESERVED2 = 0x8A,
    // Numeric
    OP_1ADD = 0x8B,
    OP_1SUB = 0x8C,
    OP_2MUL = 0x8D,
    OP_2DIV = 0x8E,
    OP_NEGATE = 0x8F,
    OP_ABS = 0x90,
    OP_NOT = 0x91,
    OP_0NOTEQUAL = 0x92,
    OP_ADD = 0x93,
    OP_SUB = 0x94,
    OP_MUL = 0x95,
    OP_DIV = 0x96,
    OP_MOD = 0x97,
    OP_LSHIFT = 0x98,
    OP_RSHIFT = 0x99,
    OP_BOOLAND = 0x9A,
    OP_BOOLOR = 0x9B,
    OP_NUMEQUAL = 0x9C,
    OP_NUMEQUALVERIFY = 0x9D,
    OP_NUMNOTEQUAL = 0x9E,
    OP_LESSTHAN = 0x9F,
    OP_GREATERTHAN = 0xA0,
    OP_LESSTHANOREQUAL = 0xA1,
    OP_GREATERTHANOREQUAL = 0xA2,
    OP_MIN = 0xA3,
    OP_MAX = 0xA4,
    OP_WITHIN = 0xA5,
    // Crypto
    OP_RIPEMD160 = 0xA6,
    OP_SHA1 = 0xA7,
    OP_SHA256 = 0xA8,
    OP_HASH160 = 0xA9,
    OP_HASH256 = 0xAA,
    OP_CODESEPARATOR = 0xAB,
    OP_CHECKSIG = 0xAC,
    OP_CHECKSIGVERIFY = 0xAD,
    OP_CHECKMULTISIG = 0xAE,
    OP_CHECKMULTISIGVERIFY = 0xAF,
    // Expansion
    OP_NOP1 = 0xB0,
    OP_CHECKLOCKTIMEVERIFY = 0xB1,
    OP_CHECKSEQUENCEVERIFY = 0xB2,
    OP_NOP4 = 0xB3,
    OP_NOP5 = 0xB4,
    OP_NOP6 = 0xB5,
    OP_NOP7 = 0xB6,
    OP_NOP8 = 0xB7,
    OP_NOP9 = 0xB8,
    OP_NOP10 = 0xB9,
    // Tapscript
    OP_CHECKSIGADD = 0xBA
  };

  // A run of script bytes: a push's data, a witness program
  struct CSpan {
    const uint8_t *Data = nullptr;
    size_t Size = 0;
  };

  // One opcode; a push hands out its data. False on a truncated script
  static bool getOp(const uint8_t *&p, const uint8_t *end, uint8_t &opcode, CSpan *data = nullptr);
  // Core's GetSigOpCount: accurate counts a multisig by its key number, otherwise as 20
  static unsigned sigOpCount(const uint8_t *p, const uint8_t *end, bool accurate);

#pragma pack(push, 1)
  struct CUnspentOutputInfo {
    // Core's Solver types, standard to spend first (isStandard compares). The record keeps only
    // what the script cannot be rebuilt without. The values are stored, and are addrdb keys
    enum EType {
      EPubKeyCompressed = 0,  // 21 <02|03 key> ac        PubKeyCompressed
      EPubKeyUncompressed,    // 41 <04 key> ac           the key in CustomData
      EPubKeyHybrid,          // 41 <06|07 key> ac        the key in CustomData
      EPubKeyHash,            // 76 a9 14 <20> 88 ac      PubKeyHash
      EScriptHash,            // a9 14 <20> 87            ScriptHash
      EMultisig,              // m <keys> n ae            the script
      EWitnessPubKeyHash,     // 00 14 <20>               WitnessProgram
      EWitnessScriptHash,     // 00 20 <32>               WitnessProgram
      EWitnessTaproot,        // 51 20 <32>               WitnessProgram
      EAnchor,                // 51 02 4e 73              nothing
      ELastStandard = EAnchor,
      EWitnessUnknown,        // v1..v16, 2..40 bytes     the script
      ENonStandard,           //                          the script
      EOpReturn               // 6a ...                   nothing, never stored
    };

    // One byte, kept as it is by the stored form of linked outputs (BTC/proto.h); four bits free
    uint8_t Type : 4;

    int64_t Value;

    union {
      uint8_t PubKeyCompressed[33];
      BaseBlob<160> PubKeyHash;
      BaseBlob<160> ScriptHash;
      uint8_t WitnessProgram[32];
      // A 65-byte key, or a whole script: its length as VarSize, then the bytes
      uint8_t CustomData[1];
    };

    // Core 28+ AreInputsStandard by the type alone; a P2SH redeem script has its own limit
    static constexpr bool isStandard(uint8_t type) { return type <= ELastStandard; }
    static size_t customDataOffset() { return offsetof(CUnspentOutputInfo, CustomData); }
  };

  static_assert(sizeof(CUnspentOutputInfo) == 1 + 8 + 33, "the flags must pack into one byte");
  static_assert(CUnspentOutputInfo::EOpReturn < 16, "the type takes four bits");
  static_assert(offsetof(CUnspentOutputInfo, Value) == BTC::Proto::CTxLinkedOutputs::AmountOffset);

  // Typed address, the addrdb/addrhistorydb key. Type is part of the key:
  // P2PKH and P2WPKH share the hash160 but are distinct addresses. P2PK folds
  // into EPubKeyHash, bare multisig is keyed by hash160 of the whole script.
  // Data is zero-padded: the struct is compared and hashed as raw bytes.
  struct CAddress {
    uint8_t Type = CUnspentOutputInfo::ENonStandard;
    uint8_t Data[32] = {};

    void set(uint8_t type, const void *data, size_t size) {
      Type = type;
      memset(Data, 0, sizeof(Data));
      memcpy(Data, data, size);
    }

    // 32 for witness script hash / taproot programs, 20 for the hash160 kinds
    size_t payloadSize() const {
      return Type == CUnspentOutputInfo::EWitnessScriptHash ||
             Type == CUnspentOutputInfo::EWitnessTaproot ? 32 : 20;
    }

    bool operator==(const CAddress &r) const { return memcmp(this, &r, sizeof(CAddress)) == 0; }
  };
#pragma pack(pop)

  // Core's IsWitnessProgram
  static bool witnessProgram(const uint8_t *script, size_t size, int &version, CSpan &program);
  // Core's Solver: CUnspentOutputInfo::EType of a script. EOpReturn is any script that starts with
  // OP_RETURN, as Core's IsUnspendable, not only NULL_DATA
  static uint8_t classify(const uint8_t *script, size_t size);
  // The script of EMultisig, EWitnessUnknown and ENonStandard, kept in the record whole
  static CSpan storedScript(const CUnspentOutputInfo &info);
  // The script a record was parsed from; false on EOpReturn, which keeps nothing
  static bool restoreScript(const CUnspentOutputInfo &info, xvector<uint8_t> &script);
  // Core's GetSigOpCount(true) of that script
  static unsigned sigOpCount(const CUnspentOutputInfo &info);

  static bool extractAddress(const BC::Proto::CTxOut &txOut, CAddress &address);
  static bool extractAddress(const CUnspentOutputInfo &info, CAddress &address);

  static std::string addressToString(const CAddress &address,
                                     const std::vector<uint8_t> &pubkeyPrefix,
                                     const std::vector<uint8_t> &scriptPrefix,
                                     const std::string &bech32Prefix);

  static bool addressFromString(const std::string &hrAddress,
                                const std::vector<uint8_t> &pubkeyPrefix,
                                const std::vector<uint8_t> &scriptPrefix,
                                const std::string &bech32Prefix,
                                CAddress &address);

  // What the engine calls: the transaction and not just the output, because a coin whose
  // outputs name their token by an index into it cannot decode one on its own. Bitcoin's
  // output stands alone, so this only picks it out - and a template because every coin on
  // this format has a transaction type of its own over the same CTxOut
  template<typename TxTy>
  static void parseTransactionOutput(const TxTy &tx, size_t outIndex, xmstream &unspentOutputInfo) {
    parseTransactionOutput(tx.TxOut[outIndex], unspentOutputInfo);
  }

  static void parseTransactionOutput(const BC::Proto::CTxOut &out, xmstream &unspentOutputInfo);

};

}

template<>
struct std::hash<BTC::Script::CAddress> {
  size_t operator()(const BTC::Script::CAddress &s) const {
    uint64_t data;
    memcpy(&data, s.Data, sizeof(data));
    return data ^ s.Type;
  }
};
