// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once
#include <asyncio/asyncioTypes.h>
#include <string.h>

struct HostAddressCompare {
  size_t operator()(const HostAddress &address) const {
    if (address.family == AF_INET) {
      return address.ipv4 ^ address.port;
    } else {
      const size_t *ipv6 = reinterpret_cast<const size_t*>(address.ipv6);
      size_t hash = 0;
      for (unsigned i = 0; i < sizeof(address.ipv6) / sizeof(size_t); i++)
        hash ^= ipv6[i];
      hash ^= address.port;
      hash ^= address.family;
      return hash;
    }
  }
};


struct HostAddressEqual {
  size_t operator()(const HostAddress &a1, const HostAddress &a2) const {
    if (a1.family == AF_INET) {
      return a1.family == a2.family &&
             a1.ipv4 == a2.ipv4 &&
             a1.port == a2.port;
    } else {
      return a1.family == a2.family &&
             a1.ipv6 == a2.ipv6 &&
             a1.port == a2.port;
    }
  }
};
