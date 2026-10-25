#pragma once
#include "common.hpp"

// Arm the server before injection. Pending reads never block window/input
// tracking, and an empty pipe is distinct from a disconnected engine.
class EnginePipe {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  OVERLAPPED connect{}, read{};
  bool connecting = false, reading = false, connected = false;
  char buffer[65536];
public:
  EnginePipe() = default;
  EnginePipe(const EnginePipe&) = delete;
  EnginePipe& operator=(const EnginePipe&) = delete;
  ~EnginePipe() { close(); }
  bool open(const std::wstring& name) {
    pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connect.hEvent || !read.hEvent) return false;
    if (ConnectNamedPipe(pipe, &connect)) connected = true;
    else {
      const auto error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) connected = true;
      else if (error == ERROR_IO_PENDING) connecting = true;
      else return false;
    }
    return true;
  }
  bool poll(std::string& chunk, DWORD& error) {
    chunk.clear(); error = ERROR_SUCCESS;
    DWORD count = 0;
    if (!connected) {
      if (!GetOverlappedResult(pipe, &connect, &count, FALSE)) {
        error = GetLastError();
        if (error == ERROR_IO_INCOMPLETE) { error = ERROR_SUCCESS; return true; }
        connecting = false; return false;
      }
      connecting = false; connected = true;
    }
    if (!reading) {
      ResetEvent(read.hEvent);
      if (ReadFile(pipe, buffer, sizeof(buffer), nullptr, &read)) {
        if (!GetOverlappedResult(pipe, &read, &count, FALSE)) { error = GetLastError(); return false; }
        if (!count) { error = ERROR_BROKEN_PIPE; return false; }
        chunk.assign(buffer, count); return true;
      }
      error = GetLastError();
      if (error == ERROR_IO_PENDING) { reading = true; error = ERROR_SUCCESS; return true; }
      return false;
    }
    if (!GetOverlappedResult(pipe, &read, &count, FALSE)) {
      error = GetLastError();
      if (error == ERROR_IO_INCOMPLETE) { error = ERROR_SUCCESS; return true; }
      reading = false; return false;
    }
    reading = false;
    if (!count) { error = ERROR_BROKEN_PIPE; return false; }
    chunk.assign(buffer, count); return true;
  }
  void close() {
    if (pipe != INVALID_HANDLE_VALUE) {
      CancelIoEx(pipe, nullptr);
      DWORD count = 0;
      if (connecting) GetOverlappedResult(pipe, &connect, &count, TRUE);
      if (reading) GetOverlappedResult(pipe, &read, &count, TRUE);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE;
    }
    if (connect.hEvent) { CloseHandle(connect.hEvent); connect.hEvent = nullptr; }
    if (read.hEvent) { CloseHandle(read.hEvent); read.hEvent = nullptr; }
    connecting = reading = connected = false;
  }
};
