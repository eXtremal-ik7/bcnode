// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// Diagnostic wrappers for third-party headers; asyncio provided these until 0.6.
#if defined(__clang__)
#define __NO_DEPRECATED_BEGIN \
  _Pragma("clang diagnostic push") \
  _Pragma("clang diagnostic ignored \"-Wdeprecated\"")
#define __NO_DEPRECATED_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define __NO_DEPRECATED_BEGIN \
  _Pragma("GCC diagnostic push") \
  _Pragma("GCC diagnostic ignored \"-Wdeprecated\"")
#define __NO_DEPRECATED_END _Pragma("GCC diagnostic pop")
#else
#define __NO_DEPRECATED_BEGIN
#define __NO_DEPRECATED_END
#endif
