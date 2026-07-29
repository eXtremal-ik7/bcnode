// Copyright (c) 2017, 2021 Pieter Wuille
// Copyright (c) 2026 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace Bech32 {

// Segwit address (BIP173/BIP350): bech32 for witness v0, bech32m for v1+.
// Returns an empty string if the program size is out of the 2..40 range.
std::string encodeSegwitAddress(const std::string &hrp, unsigned witnessVersion, const uint8_t *program, size_t programSize);

// Accepts both checksum variants and verifies the version/variant pairing.
// On success fills witnessVersion/program and returns true.
bool decodeSegwitAddress(const std::string &hrp, const std::string &address, unsigned *witnessVersion, std::vector<uint8_t> &program);

}
