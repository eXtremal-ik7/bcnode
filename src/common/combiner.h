// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <functional>

template<typename T>
class Combiner {
private:
  std::atomic<T*> Head_;

public:
  Combiner() : Head_(nullptr) {}
  void call(T *task, std::function<void(T*)> runProc) {
    // Enqueue task to stack
    T *head;

    do {
      head = Head_.load(std::memory_order_relaxed);
      task->setNext(head);
    } while (!Head_.compare_exchange_weak(head, task));

    // If previous stack was not empty, don't entry combiner
    if (head != nullptr)
      return;

    runProc(task);

    for (;;) {
      while ( (head = Head_.load(std::memory_order_relaxed)) == task) {
        if (Head_.compare_exchange_weak(head, nullptr)) {
          task->release();
          return;
        }
      }

      T *current = head;
      while (!Head_.compare_exchange_weak(current, task))
        current = Head_.load(std::memory_order_relaxed);

      // Run dequeued tasks
      while (current && current != task) {
        T *next = current->next();
        runProc(current);
        current->release();
        current = next;
      }
    }
  }
};
