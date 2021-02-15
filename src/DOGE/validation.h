#pragma once

#include "proto.h"
#include "doge.h"

bool validateAuxPow(const DOGE::Proto::Block &block, const DOGE::Common::ChainParams &chainParams, std::string &error);
