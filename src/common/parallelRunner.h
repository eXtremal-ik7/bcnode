// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// One pool serving short parallel waves the callers wait for. Two waves run at a time by design:
// the segment preparation and the existence wave of the serial stage, and the serial one is the
// critical path - hence the priority flag, whoever is free takes its chunks first. The caller
// takes a share of its own wave instead of only waiting for the helpers
class CParallelRunner {
public:
  typedef std::function<void(size_t, size_t)> CRangeProc;

  CParallelRunner() {}
  explicit CParallelRunner(unsigned threadsNum) { start(threadsNum); }
  ~CParallelRunner() { stop(); }

  void start(unsigned threadsNum) {
    for (unsigned i = 0; i < threadsNum; i++)
      Threads_.emplace_back([this]() { worker(); });
  }

  void stop() {
    if (Threads_.empty())
      return;
    {
      std::unique_lock lock(Mutex_);
      Stopped_ = true;
    }
    WaveCV_.notify_all();
    for (auto &thread: Threads_)
      thread.join();
    Threads_.clear();
  }

  unsigned threadsNum() const { return static_cast<unsigned>(Threads_.size()) + 1; }

  // Calls proc with disjoint [begin, end) ranges covering [0, count) and returns once every one
  // of them is done
  void run(size_t count, const CRangeProc &proc, bool priority = false) {
    if (!count)
      return;

    size_t parts = Threads_.size() + 1;
    if (parts == 1 || count < 2*MinPerPart) {
      proc(0, count);
      return;
    }

    CWave wave;
    wave.Proc = &proc;
    wave.Count = count;
    wave.Chunk = std::max(MinPerPart, (count + 2*parts - 1) / (2*parts));
    wave.Priority = priority;

    std::unique_lock lock(Mutex_);
    Waves_.push_back(&wave);
    WaveCV_.notify_all();

    // The caller serves its own wave only: it must not disappear into somebody else's work
    consume(lock, wave);
    DoneCV_.wait(lock, [&wave]() { return wave.Running == 0 && wave.Next >= wave.Count; });
    Waves_.erase(std::find(Waves_.begin(), Waves_.end(), &wave));
  }

private:
  // Chunks are half a share each, so a helper that wakes late still finds work
  static constexpr size_t MinPerPart = 64;

  struct CWave {
    const CRangeProc *Proc = nullptr;
    size_t Count = 0;
    size_t Chunk = 0;
    size_t Next = 0;
    size_t Running = 0;
    bool Priority = false;
  };

  // Lock held. A wave marked as the critical path wins over a background one
  CWave *pick() {
    CWave *found = nullptr;
    for (CWave *wave: Waves_) {
      if (wave->Next >= wave->Count)
        continue;
      if (!found || (wave->Priority && !found->Priority))
        found = wave;
    }

    return found;
  }

  void worker() {
    std::unique_lock lock(Mutex_);
    for (;;) {
      WaveCV_.wait(lock, [this]() { return Stopped_ || pick() != nullptr; });
      if (Stopped_)
        return;

      while (CWave *wave = pick())
        consume(lock, *wave);
    }
  }

  // Takes chunks of a wave until it runs out; the lock is held on entry and on exit and dropped
  // for the calls themselves
  void consume(std::unique_lock<std::mutex> &lock, CWave &wave) {
    const CRangeProc &proc = *wave.Proc;
    while (wave.Next < wave.Count) {
      size_t begin = wave.Next;
      size_t end = std::min(begin + wave.Chunk, wave.Count);
      wave.Next = end;
      wave.Running++;
      lock.unlock();
      proc(begin, end);
      lock.lock();
      wave.Running--;
      if (!wave.Running && wave.Next >= wave.Count)
        DoneCV_.notify_all();
    }
  }

private:
  std::mutex Mutex_;
  std::condition_variable WaveCV_;
  std::condition_variable DoneCV_;
  std::vector<std::thread> Threads_;
  bool Stopped_ = false;

  // Active waves; each one is owned by the thread inside run()
  std::vector<CWave*> Waves_;
};
