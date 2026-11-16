#pragma once

#include <cstdint>

namespace BTC {

// Core's SCRIPT_VERIFY_* bits with Core's values: test vectors and the coins' flag sets map one to one
enum EScriptFlag : uint32_t {
  EScriptNone = 0,
  EScriptP2SH = 1u << 0,
  EScriptStrictEnc = 1u << 1,
  EScriptDerSig = 1u << 2,
  EScriptLowS = 1u << 3,
  EScriptNullDummy = 1u << 4,
  EScriptSigPushOnly = 1u << 5,
  EScriptMinimalData = 1u << 6,
  EScriptDiscourageUpgradableNops = 1u << 7,
  EScriptCleanStack = 1u << 8,
  EScriptCheckLockTimeVerify = 1u << 9,
  EScriptCheckSequenceVerify = 1u << 10,
  EScriptWitness = 1u << 11,
  EScriptDiscourageUpgradableWitnessProgram = 1u << 12,
  EScriptMinimalIf = 1u << 13,
  EScriptNullFail = 1u << 14,
  EScriptWitnessPubKeyType = 1u << 15,
  EScriptConstScriptCode = 1u << 16,
  EScriptTaproot = 1u << 17,
  EScriptDiscourageUpgradableTaprootVersion = 1u << 18,
  EScriptDiscourageOpSuccess = 1u << 19,
  EScriptDiscourageUpgradablePubKeyType = 1u << 20
};

}
