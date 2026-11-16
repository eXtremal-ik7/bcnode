#pragma once

#include "script.h"
#include "scriptError.h"
#include "scriptFlags.h"

// Core's checks under the flags (script/interpreter.cpp): which ones apply is the flags' business
// alone, the fast path and the interpreter share them

namespace BTC {

// Core's MAX_SCRIPT_ELEMENT_SIZE: a push, a witness element of segwit v0
constexpr size_t MaxScriptElementSize = 520;

// Core's SigVersion
enum class ESigVersion : uint8_t {
  Base,
  WitnessV0,
  Taproot,
  Tapscript
};

// Core's CastToBool: zeros are false, negative zero (0x80 last) too
bool castToBool(const uint8_t *data, size_t size);

// Core's CheckMinimalPush, for MINIMALDATA: data pushed by opcode, OP_0 to OP_PUSHDATA4
bool checkMinimalPush(Script::CSpan data, uint8_t opcode);

// Core's CheckSignatureEncoding: DERSIG (BIP66), LOW_S, STRICTENC. The signature with its hash
// type byte; an empty one passes, CHECKSIG fails it
EScriptError checkSignatureEncoding(Script::CSpan signature, uint32_t flags);

// Core's CheckPubKeyEncoding: STRICTENC, WITNESS_PUBKEYTYPE for segwit v0
EScriptError checkPubKeyEncoding(Script::CSpan key, uint32_t flags, ESigVersion version);

// CPubKey::Verify's parsing: r and s of a DER signature (no hash type byte) as lax as Core takes
// it, S normalized, libsecp256k1 verifying low S only. False where it does not parse
bool ecdsaCompact(Script::CSpan der, uint8_t (&compact)[64]);

// Core's CheckSchnorrSignature ahead of the verification: the size, the hash type (an explicit
// SIGHASH_DEFAULT, an undefined one, SIGHASH_SINGLE without the output of its index fail).
// hashType is the signature's, SIGHASH_DEFAULT for 64 bytes
EScriptError checkSchnorrSignature(Script::CSpan signature, bool hasOutput, uint8_t &hashType);

}
