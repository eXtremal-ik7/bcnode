// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "p2putils/xmstream.h"
#include "xvector.h"

template<typename T>
static inline void xvectorFromStream(xmstream &&src, xvector<T> &dst) {
  size_t size = src.sizeOf();
  size_t msize = src.capacity();
  size_t own = src.own();
  dst.set(static_cast<T*>(src.capture()), size, msize, own);
}
