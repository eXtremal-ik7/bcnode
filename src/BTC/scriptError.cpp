#include "scriptError.h"

namespace BTC {

const char *scriptErrorString(EScriptError error)
{
  switch (error) {
    case EScriptError::Ok : return "No error";
    case EScriptError::EvalFalse : return "Script evaluated without error but finished with a false/empty top stack element";
    case EScriptError::Verify : return "Script failed an OP_VERIFY operation";
    case EScriptError::EqualVerify : return "Script failed an OP_EQUALVERIFY operation";
    case EScriptError::CheckMultisigVerify : return "Script failed an OP_CHECKMULTISIGVERIFY operation";
    case EScriptError::CheckSigVerify : return "Script failed an OP_CHECKSIGVERIFY operation";
    case EScriptError::NumEqualVerify : return "Script failed an OP_NUMEQUALVERIFY operation";
    case EScriptError::ScriptSize : return "Script is too big";
    case EScriptError::PushSize : return "Push value size limit exceeded";
    case EScriptError::OpCount : return "Operation limit exceeded";
    case EScriptError::StackSize : return "Stack size limit exceeded";
    case EScriptError::SigCount : return "Signature count negative or greater than pubkey count";
    case EScriptError::PubKeyCount : return "Pubkey count negative or limit exceeded";
    case EScriptError::BadOpcode : return "Opcode missing or not understood";
    case EScriptError::DisabledOpcode : return "Attempted to use a disabled opcode";
    case EScriptError::InvalidStackOperation : return "Operation not valid with the current stack size";
    case EScriptError::InvalidAltstackOperation : return "Operation not valid with the current altstack size";
    case EScriptError::OpReturn : return "OP_RETURN was encountered";
    case EScriptError::UnbalancedConditional : return "Invalid OP_IF construction";
    case EScriptError::NegativeLocktime : return "Negative locktime";
    case EScriptError::UnsatisfiedLocktime : return "Locktime requirement not satisfied";
    case EScriptError::SigHashtype : return "Signature hash type missing or not understood";
    case EScriptError::SigDer : return "Non-canonical DER signature";
    case EScriptError::MinimalData : return "Data push larger than necessary";
    case EScriptError::SigPushOnly : return "Only push operators allowed in signatures";
    case EScriptError::SigHighS : return "Non-canonical signature: S value is unnecessarily high";
    case EScriptError::SigNullDummy : return "Dummy CHECKMULTISIG argument must be zero";
    case EScriptError::MinimalIf : return "OP_IF/NOTIF argument must be minimal";
    case EScriptError::SigNullFail : return "Signature must be zero for failed CHECK(MULTI)SIG operation";
    case EScriptError::DiscourageUpgradableNops : return "NOPx reserved for soft-fork upgrades";
    case EScriptError::DiscourageUpgradableWitnessProgram : return "Witness version reserved for soft-fork upgrades";
    case EScriptError::DiscourageUpgradableTaprootVersion : return "Taproot version reserved for soft-fork upgrades";
    case EScriptError::DiscourageOpSuccess : return "OP_SUCCESSx reserved for soft-fork upgrades";
    case EScriptError::DiscourageUpgradablePubKeyType : return "Public key version reserved for soft-fork upgrades";
    case EScriptError::PubKeyType : return "Public key is neither compressed or uncompressed";
    case EScriptError::CleanStack : return "Stack size must be exactly one after execution";
    case EScriptError::WitnessProgramWrongLength : return "Witness program has incorrect length";
    case EScriptError::WitnessProgramWitnessEmpty : return "Witness program was passed an empty witness";
    case EScriptError::WitnessProgramMismatch : return "Witness program hash mismatch";
    case EScriptError::WitnessMalleated : return "Witness requires empty scriptSig";
    case EScriptError::WitnessMalleatedP2SH : return "Witness requires only-redeemscript scriptSig";
    case EScriptError::WitnessUnexpected : return "Witness provided for non-witness script";
    case EScriptError::WitnessPubKeyType : return "Using non-compressed keys in segwit";
    case EScriptError::SchnorrSigSize : return "Invalid Schnorr signature size";
    case EScriptError::SchnorrSigHashtype : return "Invalid Schnorr signature hash type";
    case EScriptError::SchnorrSig : return "Invalid Schnorr signature";
    case EScriptError::TaprootWrongControlSize : return "Invalid Taproot control block size";
    case EScriptError::TapscriptValidationWeight : return "Too much signature validation relative to witness weight";
    case EScriptError::TapscriptCheckMultisig : return "OP_CHECKMULTISIG(VERIFY) is not available in tapscript";
    case EScriptError::TapscriptMinimalIf : return "OP_IF/NOTIF argument must be minimal in tapscript";
    case EScriptError::OpCodeSeparator : return "Using OP_CODESEPARATOR in non-witness script";
    case EScriptError::SigFindAndDelete : return "Signature is found in scriptCode";
    case EScriptError::UnknownError :
    default :
      break;
  }
  return "unknown error";
}

}
