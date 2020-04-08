// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "thread.h"
#include <asyncio/asyncioTypes.h>
#include <atomic>
 
static std::atomic<unsigned> threadCounter = 0;
static __tls unsigned threadId;

void InitializeWorkerThread()
{
  threadId = threadCounter.fetch_add(1);
}

unsigned GetWorkerThreadId()
{
  return threadId;
}
