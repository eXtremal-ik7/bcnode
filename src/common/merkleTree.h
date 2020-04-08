// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "BC/proto.h"

BC::Proto::BlockHashTy calculateMerkleRoot(const xvector<BC::Proto::Transaction> &vtx);
