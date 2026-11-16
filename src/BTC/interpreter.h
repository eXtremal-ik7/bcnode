#pragma once

#include "sigCheck.h"
#include <cstddef>
#include <cstdint>

// Core's VerifyScript (script/interpreter.cpp) for the spends off the fast path's templates
//: EvalScript, P2SH, witness v0, tapscript. Signatures are checked at
// once, whatever the mode of the fast path

namespace BTC {

// One input under the flags, Ok when the spend is valid. checker is the transaction's CTxChecker.
// A stub: nothing is ported yet and every spend passes unchecked. The verdict says Interpreted, so
// a caller counts what it took on trust
template<typename CheckerTy>
EScriptError interpretInput(CheckerTy&, size_t, uint32_t)
{
  return EScriptError::Ok;
}

}
