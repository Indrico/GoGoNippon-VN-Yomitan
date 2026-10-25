#pragma once
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct StudyInit {
  DWORD size;
  wchar_t pipe[128];
  wchar_t directory[1024];
  wchar_t profile[1024];
};
inline std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, '\0');
  if (n) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
  return out;
}
inline std::wstring wide(const std::string& value, UINT cp = CP_UTF8) {
  if (value.empty()) return {};
  int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  if (n) MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
  return out;
}
inline std::string quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c < 0x20) { char hex[7]; std::snprintf(hex, sizeof(hex), "\\u%04x", c); out += hex; }
    else out += c;
  }
  return out + '"';
}
inline bool readMemory(const void* source, void* destination, size_t bytes) {
  SIZE_T actual = 0;
  return ReadProcessMemory(GetCurrentProcess(), source, destination, bytes, &actual) && actual == bytes;
}
inline std::string hexBytes(const void* source, size_t count) {
  std::vector<unsigned char> data(count);
  if (!readMemory(source, data.data(), count)) return {};
  const char* digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : data) { out += digits[c >> 4]; out += digits[c & 15]; }
  return out;
}
inline std::string errorMessage(DWORD error = GetLastError()) {
  wchar_t* buffer = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::string out = buffer ? utf8(buffer) : "Windows error " + std::to_string(error);
  if (buffer) LocalFree(buffer);
  return out;
}
