// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>

// Flat combining with a whole-list drain: producers push a node and leave, one thread takes
// the owner role and runs the sequential state machine on its private data. Unlike
// combiner.h the role is a separate word, so a thread with no node of its own (a flush
// request, a timer) can enter as well.
//
// Nodes leave the list only with the whole list (exchange), never one by one: no ABA, no
// reclamation problem, and the owner is free to reuse or delete what it took.
//
// No work is lost: a producer that failed to take the role had read Owner_ == 1 before the
// owner's release, so its push precedes the owner's post-release recheck of the list in the
// seq_cst order - the owner sees the node and takes the role again. Both ends of that
// argument (push CAS, recheck load) are seq_cst on purpose.
//
// Sleeping is not this class' business: whoever must wait does it on a kernel object
template<typename T>
class CInbox {
public:
  // Any thread; T needs a 'T *Next' member
  void push(T *node) {
    T *head = Head_.load(std::memory_order_relaxed);
    do {
      node->Next = head;
    } while (!Head_.compare_exchange_weak(head, node));
  }

  bool tryAcquire() {
    uint32_t free = 0;
    return Owner_.compare_exchange_strong(free, 1);
  }

  // Owner only: the whole list at once, back in push order
  T *take() {
    T *list = Head_.exchange(nullptr, std::memory_order_acquire);
    T *result = nullptr;
    while (list) {
      T *next = list->Next;
      list->Next = result;
      result = list;
      list = next;
    }

    return result;
  }

  // Owner only: gives the role back. Returns true when work appeared and the role was taken
  // again, so the caller keeps sequencing. 'pending' is evaluated after the release: a
  // request published by a thread that lost the role race must not be missed
  template<typename F> bool release(F pending) {
    Owner_.store(0);
    if (Head_.load() == nullptr && !pending())
      return false;

    return tryAcquire();
  }

  bool empty() const { return Head_.load(std::memory_order_relaxed) == nullptr; }

private:
  std::atomic<T*> Head_ = nullptr;
  std::atomic<uint32_t> Owner_ = 0;
};
