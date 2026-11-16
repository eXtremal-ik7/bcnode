// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// The engine's side of the script checks of blocks. Which blocks are checked and how far is the
// engine's choice; the scripts themselves are the coin's, BC::Common::checkBlockScripts

#include <cstdint>
#include <string>

// What the scripts of a block are checked for
enum class EScriptCheck {
  // Nothing: every spend is taken as valid
  Off,
  // A block new to this node in full; one read from our own files by every rule but the
  // signatures, taken on trust
  New,
  // Every block in full
  All
};

// What a check of a block's transactions came to
struct CScriptCheckResult {
  uint64_t Inputs = 0;
  // Signatures verified, not taken on trust
  uint64_t Signatures = 0;
  // Inputs the coin has nothing to check with yet, taken as valid
  uint64_t Unchecked = 0;
  // The first invalid input and why, when the check fails
  uint32_t Tx = 0;
  uint32_t Input = 0;
  std::string Error;
};

// A bitmap of a block's transactions the engine has checked already: bit i is transaction i
inline bool scriptTxSkipped(const uint64_t *skip, size_t tx)
{
  return skip && (skip[tx >> 6] >> (tx & 63) & 1);
}
