// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "linearDataStorage.h"
#include "loguru.hpp"
#include "asyncio/asyncioTypes.h"
#include <system_error>
#include <vector>

static thread_local std::vector<FILE*> allFds;

void LinearDataReader::init(const std::filesystem::path &directory, const char *format)
{
  _directory = directory;
  _format = format;
}

std::filesystem::path LinearDataReader::getFilePath(uint32_t fileNo)
{
  char fileName[64];
  snprintf(fileName, sizeof(fileName), _format, fileNo);
  std::filesystem::path path = _directory / fileName;
  return path;
}

bool LinearDataReader::read(uint32_t fileNo, uint32_t offset, void *data, uint32_t size)
{
  if (allFds.size() <= fileNo)
    allFds.resize(fileNo+1, nullptr);

  if (allFds[fileNo] == nullptr) {
    auto path = getFilePath(fileNo);
    allFds[fileNo] = fopen(path.u8string().c_str(), "rb");
    if (!allFds[fileNo]) {
      LOG_F(ERROR, "Can't open file %s for reading", path.c_str());
      return false;
    }
  }

  FILE *hFile = allFds[fileNo];
  fseek(hFile, offset, SEEK_SET);
  if (fread(data, size, 1, hFile) != 1) {
    LOG_F(ERROR, "Can't read data from %s (offset = %u, size = %u)", getFilePath(fileNo).c_str(), offset, size);
    return false;
  }

  return true;
}

bool LinearDataWriter::flush()
{
  if (_data.sizeOf() == 0) {
    _lastWritePoint = std::chrono::steady_clock::now();
    return true;
  }

  if (ftell(_hFile) != _fileOffset) {
    fseek(_hFile, _fileOffset, SEEK_SET);
    if (ftell(_hFile) != _fileOffset) {
      LOG_F(ERROR, "Can't write data to blk%05u.dat", _fileNo);
      return false;
    }
  }

  if (fwrite(_data.data(), _data.sizeOf(), 1, _hFile) != 1) {
    LOG_F(ERROR, "Can't write data to blk%05u.dat", _fileNo);
    return false;
  }

  fflush(_hFile);
  _fileOffset += static_cast<uint32_t>(_data.sizeOf());
  _data.reset();
  _lastWritePoint = std::chrono::steady_clock::now();
  return true;
}

bool LinearDataWriter::init(const std::filesystem::path &directory, const char *format, uint32_t fileSizeLimit)
{
  _directory = directory;
  _format = format;
  _fileSizeLimit = fileSizeLimit;

  char fileName[64];
  std::error_code errorCode;
  if (!std::filesystem::create_directory(_directory, errorCode) && errorCode.value() != 0) {
    LOG_F(ERROR, "Cant create directory %s (%s)", _directory.u8string().c_str(), errorCode.message().c_str());
    return false;
  }

  uint32_t fileNo = 0;
  uint32_t fileSize = 0;
  for (;;) {
    snprintf(fileName, sizeof(fileName), _format, fileNo);
    std::filesystem::path path = _directory / fileName;
    if (!std::filesystem::exists(path))
      break;

    fileSize = static_cast<uint32_t>(std::filesystem::file_size(path));
    fileNo++;
  }

  if (fileNo == 0 || fileSize >= _fileSizeLimit) {
    _fileNo = fileNo;
    _fileOffset = 0;
  } else {
    _fileNo = fileNo - 1;
    _fileOffset = fileSize;
  }

  {
    // Open current storage file
    snprintf(fileName, sizeof(fileName), _format, _fileNo);
    std::filesystem::path path = _directory / fileName;
    FILE *hFile = fopen(path.u8string().c_str(), "ab");
    if (hFile) {
      fseek(hFile, _fileOffset, SEEK_SET);
      _hFile = hFile;
    } else {
      LOG_F(ERROR, "Can't open block storage file %s for writting", path.u8string().c_str());
      return false;
    }

    LOG_F(INFO, "Storage inititialized, current file/offset: %s/%u", path.u8string().c_str(), _fileOffset);
  }

  _lastWritePoint = std::chrono::steady_clock::now();
  return true;
}

bool LinearDataWriter::write(void *prefix, uint32_t prefixSize, void *data, uint32_t size, std::pair<uint32_t, uint32_t> &position)
{
  position.first = _fileNo;
  position.second = _fileOffset + static_cast<uint32_t>(_data.sizeOf());
  _data.write(prefix, prefixSize);
  _data.write(data, size);

  if (_fileOffset + _data.sizeOf() > _fileSizeLimit) {
    if (!flush())
      return false;
    fclose(_hFile);
    _fileNo++;
    _fileOffset = 0;

    char fileName[64];
    snprintf(fileName, sizeof(fileName), _format, _fileNo);
    std::filesystem::path blocksPath = _directory / fileName;
    _hFile = fopen(blocksPath.u8string().c_str(), "wb");
    if (!_hFile) {
      LOG_F(ERROR, "Can't open block data file %s", blocksPath.c_str());
      return false;
    }
  } else if (_data.sizeOf() >= 4*1048576) {
    if (!flush())
      return false;
  }

  return true;
}
