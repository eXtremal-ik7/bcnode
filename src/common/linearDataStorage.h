// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <p2putils/xmstream.h>
#include <chrono>
#include <filesystem>

class LinearDataReader {
private:
  std::filesystem::path _directory;
  const char *_format = nullptr;

public:
  void init(const std::filesystem::path &directory, const char *format);
  std::filesystem::path getFilePath(uint32_t fileNo);
  bool read(uint32_t fileNo, uint32_t offset, void *data, uint32_t size);
};

class LinearDataWriter {
private:
  std::filesystem::path _directory;
  const char *_format = nullptr;
  FILE *_hFile = nullptr;
  uint32_t _fileNo = 0;
  uint32_t _fileOffset = 0;
  uint32_t _fileSizeLimit = 0;
  xmstream _data;
  std::chrono::time_point<std::chrono::steady_clock> _lastWritePoint = std::chrono::time_point<std::chrono::steady_clock>::max();

public:
  ~LinearDataWriter() {
    if (_hFile)
      fclose(_hFile);
  }

  bool init(const std::filesystem::path &directory, const char *format, uint32_t fileSizeLimit);
  bool write(void *prefix, uint32_t prefixSize, void *data, uint32_t size, std::pair<uint32_t, uint32_t> &position);
  bool flush();
  bool empty() const { return _fileNo == 0 && _fileOffset == 0; }
  bool bufferEmpty() { return _data.sizeOf() == 0; }
  std::chrono::time_point<std::chrono::steady_clock> lastWriteTime() { return _lastWritePoint; }
};
