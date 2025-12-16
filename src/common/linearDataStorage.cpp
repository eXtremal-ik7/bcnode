// Copyright (c) 2020 Ivan K.
// Copyright (c) 2020 The BCNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "linearDataStorage.h"
#include "loguru.hpp"
#include <system_error>


std::filesystem::path LinearDataStorage::getFilePath(uint32_t fileNo)
{
  char fileName[64];
  snprintf(fileName, sizeof(fileName), Format_, fileNo);
  std::filesystem::path path = Directory_ / fileName;
  return path;
}

FileDescriptor LinearDataStorage::file(uint32_t fileNo)
{
  if (!Files_[fileNo].isOpened()) {
    auto path = getFilePath(fileNo);
    if (!Files_[fileNo].open(path))
      LOG_F(ERROR, "Can't open block data file %s", path.u8string().c_str());
  }

  return Files_[fileNo];
}

bool LinearDataStorage::flush()
{
  if (Data_.sizeOf() == 0) {
    LastWritePoint_ = std::chrono::steady_clock::now();
    return true;
  }

  FileDescriptor hFile = file(FileNo_);
  auto result = hFile.write(Data_.data(), FileOffset_, Data_.sizeOf());
  if (result < 0 || static_cast<size_t>(result) != Data_.sizeOf()) {
    LOG_F(ERROR, "Can't write data to %s", getFilePath(FileNo_).u8string().c_str());
    return false;
  }

  FileOffset_ += static_cast<uint32_t>(Data_.sizeOf());
  Data_.reset();
  LastWritePoint_ = std::chrono::steady_clock::now();
  return true;
}

bool LinearDataStorage::init(const std::filesystem::path &directory, const char *format, uint32_t fileSizeLimit, uint32_t fileNumberLimit)
{
  FileNumberLimit_ = fileNumberLimit;
  Files_.reset(new FileDescriptor[fileNumberLimit]);

  Directory_ = directory;
  Format_ = format;
  FileSizeLimit_ = fileSizeLimit;

  char fileName[64];
  std::error_code errorCode;
  if (!std::filesystem::create_directory(Directory_, errorCode) && errorCode.value() != 0) {
    LOG_F(ERROR, "Cant create directory %s (%s)", Directory_.u8string().c_str(), errorCode.message().c_str());
    return false;
  }

  uint32_t fileNo = 0;
  uint32_t fileSize = 0;
  for (;;) {
    snprintf(fileName, sizeof(fileName), Format_, fileNo);
    std::filesystem::path path = Directory_ / fileName;
    if (!std::filesystem::exists(path))
      break;

    fileSize = static_cast<uint32_t>(std::filesystem::file_size(path));
    fileNo++;
  }

  if (fileNo == 0 || fileSize >= FileSizeLimit_) {
    FileNo_ = fileNo;
    FileOffset_ = 0;
  } else {
    FileNo_ = fileNo - 1;
    FileOffset_ = fileSize;
  }

  {
    // Open current storage file
    snprintf(fileName, sizeof(fileName), Format_, FileNo_);
    std::filesystem::path path = Directory_ / fileName;
    FileDescriptor hFile = file(FileNo_);
    if (!hFile.isOpened()) {
      LOG_F(ERROR, "Can't open block storage file %s for writting", path.u8string().c_str());
      return false;
    }

    LOG_F(INFO, "Storage inititialized, current file/offset: %s/%u", path.u8string().c_str(), FileOffset_);
  }

  LastWritePoint_ = std::chrono::steady_clock::now();
  return true;
}

bool LinearDataStorage::read(uint32_t fileNo, uint32_t offset, void *data, uint32_t size)
{
  FileDescriptor hFile = file(fileNo);
  ssize_t result = hFile.read(data, offset, size);
  return result > 0 && static_cast<size_t>(result) == size;
}

bool LinearDataStorage::write(uint32_t fileNo, uint32_t offset, const void *data, size_t size)
{
  ssize_t result = file(fileNo).write(data, offset, size);
  return result > 0 && static_cast<size_t>(result) == size;
}

bool LinearDataStorage::allocate(uint32_t size, std::pair<uint32_t, uint32_t> &position)
{
  flush();
  if (FileOffset_ + size > FileSizeLimit_) {
    FileNo_++;
    FileOffset_ = size;
  } else {
    FileOffset_ += size;
  }

  FileDescriptor hFile = file(FileNo_);
  if (!hFile.isOpened())
    return false;
  if (!hFile.truncate(FileOffset_)) {
    LOG_F(ERROR, "Can't open block storage file %s for writting", getFilePath(FileNo_).u8string().c_str());
    return false;
  }
  position.first = FileNo_;
  position.second = FileOffset_ - size;
  return true;
}

bool LinearDataStorage::copy(uint32_t srcFileNo, uint32_t srcOffset, uint32_t dstFileNo, uint32_t dstOffset, uint32_t size)
{
  // Use 64Mb buffer
  constexpr uint32_t bufferSize = 64u << 20;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufferSize]);

  FileDescriptor src = file(srcFileNo);
  FileDescriptor dst = file(dstFileNo);
  if (!src.isOpened() || !dst.isOpened())
    return false;

  ssize_t result;
  uint32_t remaining = size;
  while (remaining) {
    uint32_t currentSize = std::min(bufferSize, size);

    result = src.read(buffer.get(), srcOffset, currentSize);
    if (result < 0 || static_cast<size_t>(result) != currentSize) {
      return false;
    }

    result = dst.write(buffer.get(), dstOffset, currentSize);
    if (result < 0 || static_cast<size_t>(result) != currentSize) {
      return false;
    }

    srcOffset += currentSize;
    dstOffset += currentSize;
    remaining -= currentSize;
  }

  return true;
}

bool LinearDataStorage::append2(void *prefix, uint32_t prefixSize, void *data, uint32_t size, std::pair<uint32_t, uint32_t> &position)
{
  position.first = FileNo_;
  position.second = FileOffset_ + static_cast<uint32_t>(Data_.sizeOf());
  Data_.write(prefix, prefixSize);
  Data_.write(data, size);

  if (FileOffset_ + Data_.sizeOf() > FileSizeLimit_) {
    if (!flush())
      return false;

    FileNo_++;
    FileOffset_ = 0;
    if (!file(FileNo_).isOpened())
      return false;
  } else if (Data_.sizeOf() >= 32*1048576) {
    if (!flush())
      return false;
  }

  return true;
}
