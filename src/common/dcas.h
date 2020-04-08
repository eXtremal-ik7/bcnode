// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef _MSC_VER
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Winsock2.h>
#endif

#ifdef __clang__
_Pragma("clang diagnostic push")
_Pragma("clang diagnostic ignored \"-Wunused-function\"")
#endif

template<int>
inline bool dcas(void *ptr, void *oldValue, void *newValue);

template<>
inline bool dcas<4>(void *ptr, void *oldValue, void *newValue)
{
#ifndef _MSC_VER
  return __atomic_compare_exchange(static_cast<uint64_t*>(ptr), static_cast<uint64_t*>(oldValue), static_cast<uint64_t*>(newValue), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
  return InterlockedCompareExchange64(static_cast<LONG64*>(ptr), *static_cast<LONG64*>(newValue), *static_cast<LONG64*>(oldValue));
#endif
}
template<>
inline bool dcas<8>(void *ptr, void *oldValue, void *newValue)
{
#ifndef _MSC_VER
  return __atomic_compare_exchange(static_cast<unsigned __int128*>(ptr), static_cast<unsigned __int128*>(oldValue), static_cast<unsigned __int128*>(newValue), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#else
  LONG64* exchangePtr = static_cast<LONG64*>(newValue);
  return InterlockedCompareExchange128(static_cast<LONG64*>(ptr), exchangePtr[1], exchangePtr[0], static_cast<LONG64*>(oldValue));
#endif
}

static inline bool dblCompareAndExchange(void *ptr, void *oldValue, void *newValue)
{
  return dcas<sizeof(size_t)>(ptr, oldValue, newValue);
}

#ifdef __clang__
_Pragma("clang diagnostic pop")
#endif
