// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once
#include <asyncio/asyncioTypes.h>
#include <string.h>

struct HostAddressCompare {
  size_t operator()(const HostAddress &address) const {
    const size_t *ipv6 = reinterpret_cast<const size_t*>(address.ipv6);
    size_t hash = 0;
    for (unsigned i = 0; i < sizeof(address.ipv6) / sizeof(size_t); i++)
      hash ^= ipv6[i];
    hash ^= address.port;
    hash ^= address.family;
    return hash;
  }
};


struct HostAddressEqual {
  size_t operator()(const HostAddress &a1, const HostAddress &a2) const {
    return memcmp(&a1, &a2, sizeof(HostAddress)) == 0;
  }
};
