// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "common/file.h"
#include <p2putils/xmstream.h>
#include <chrono>
#include <filesystem>


class LinearDataStorage {
private:
  std::filesystem::path Directory_;
  const char *Format_ = nullptr;
  std::unique_ptr<FileDescriptor[]> Files_;
  uint32_t FileNumberLimit_ = 0;
  uint32_t FileNo_ = 0;
  uint32_t FileOffset_ = 0;
  uint32_t FileSizeLimit_ = 0;
  xmstream Data_;
  std::chrono::time_point<std::chrono::steady_clock> LastWritePoint_ = std::chrono::time_point<std::chrono::steady_clock>::max();

private:
  FileDescriptor file(uint32_t fileNo);

public:
  ~LinearDataStorage() {
    for (uint32_t i = 0; i < FileNumberLimit_; i++)
      Files_[i].close();
  }

  bool init(const std::filesystem::path &directory, const char *format, uint32_t fileSizeLimit, uint32_t fileNumberLimit = 65536);

  std::filesystem::path getFilePath(uint32_t fileNo);

  bool read(uint32_t fileNo, uint32_t offset, void *data, uint32_t size);
  bool write(uint32_t fileNo, uint32_t offset, const void *data, size_t size);
  bool allocate(uint32_t size, std::pair<uint32_t, uint32_t> &position);
  bool copy(uint32_t srcFileNo, uint32_t srcOffset, uint32_t dstFileNo, uint32_t dstOffset, uint32_t size);
  bool append2(void *prefix, uint32_t prefixSize, void *data, uint32_t size, std::pair<uint32_t, uint32_t> &position);

  bool flush();
  bool empty() const { return FileNo_ == 0 && FileOffset_ == 0; }
  bool bufferEmpty() { return Data_.sizeOf() == 0; }
  std::chrono::time_point<std::chrono::steady_clock> lastWriteTime() { return LastWritePoint_; }
};
