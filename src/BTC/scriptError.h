#pragma once

#include <cstdint>

namespace BTC {

// Core's ScriptError_t, in Core's order: a reject reason is Core's string for it
enum class EScriptError : uint8_t {
  Ok = 0,
  UnknownError,
  EvalFalse,
  OpReturn,
  // Max sizes
  ScriptSize,
  PushSize,
  OpCount,
  StackSize,
  SigCount,
  PubKeyCount,
  // Failed verify operations
  Verify,
  EqualVerify,
  CheckMultisigVerify,
  CheckSigVerify,
  NumEqualVerify,
  // Logical, format, canonical errors
  BadOpcode,
  DisabledOpcode,
  InvalidStackOperation,
  InvalidAltstackOperation,
  UnbalancedConditional,
  // CHECKLOCKTIMEVERIFY and CHECKSEQUENCEVERIFY
  NegativeLocktime,
  UnsatisfiedLocktime,
  // Malleability
  SigHashtype,
  SigDer,
  MinimalData,
  SigPushOnly,
  SigHighS,
  SigNullDummy,
  PubKeyType,
  CleanStack,
  MinimalIf,
  SigNullFail,
  // Softfork safeness
  DiscourageUpgradableNops,
  DiscourageUpgradableWitnessProgram,
  DiscourageUpgradableTaprootVersion,
  DiscourageOpSuccess,
  DiscourageUpgradablePubKeyType,
  // Segregated witness
  WitnessProgramWrongLength,
  WitnessProgramWitnessEmpty,
  WitnessProgramMismatch,
  WitnessMalleated,
  WitnessMalleatedP2SH,
  WitnessUnexpected,
  WitnessPubKeyType,
  // Taproot
  SchnorrSigSize,
  SchnorrSigHashtype,
  SchnorrSig,
  TaprootWrongControlSize,
  TapscriptValidationWeight,
  TapscriptCheckMultisig,
  TapscriptMinimalIf,
  // Constant scriptCode
  OpCodeSeparator,
  SigFindAndDelete
};

// Core's ScriptErrorString
const char *scriptErrorString(EScriptError error);

// Why the mempool refuses a transaction's scripts
enum class EScriptRefusal : uint8_t {
  None,
  // Fails the standard flags alone: valid in a block
  NonStandard,
  // Fails the flags a block is checked under: the sender is at fault
  Mandatory,
  // A witness spend that came without its witness: the transaction itself may be fine, so its
  // txid is not to be remembered as refused (Core's TX_WITNESS_STRIPPED)
  WitnessStripped
};

}
