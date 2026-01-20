#pragma once

#include <filesystem>

inline FILE* fopen_path(const std::filesystem::path& path, const char* mode)
{
#ifdef _WIN32
  wchar_t wmode[8];
  mbstowcs(wmode, mode, 8);
  return _wfopen(path.c_str(), wmode);
#else
  return fopen(path.c_str(), mode);
#endif
}
