#include "common.hpp"
#include "engine-pipe.hpp"
#include <tlhelp32.h>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <iostream>
#include <mutex>
#include <fstream>

static std::mutex outputMutex;
static std::atomic<bool> done{false}, snapshotRequested{false};
static DWORD targetPid = 0;
static HWND targetWindow = nullptr;
static HANDLE targetProcess = nullptr;
static std::wstring directory;
static void emit(const std::string& message) {
  std::lock_guard<std::mutex> lock(outputMutex);
  std::cout << message << '\n' << std::flush;
}
static void status(const std::string& message, const std::string& detail = "") {
  emit("{\"type\":\"status\",\"message\":" + quote(message) + ",\"detail\":" + quote(detail) + "}");
}
static std::wstring canonicalPath(const std::wstring& path) {
  std::wstring full(32768, L'\0');
  DWORD count = GetFullPathNameW(path.c_str(), full.size(), full.data(), nullptr);
  if (!count || count >= full.size()) return path;
  full.resize(count);
  HANDLE file = CreateFileW(full.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    std::wstring final(32768, L'\0');
    count = GetFinalPathNameByHandleW(file, final.data(), final.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (count && count < final.size()) { final.resize(count); full = std::move(final); }
  }
  if (full.compare(0, 8, L"\\\\?\\UNC\\") == 0) full = L"\\\\" + full.substr(8);
  else if (full.compare(0, 4, L"\\\\?\\") == 0) full.erase(0, 4);
  return full;
}
struct GameMatch { DWORD pid = 0, scanError = 0; HWND window = nullptr; LONG area = 0; bool denied = false; };
static BOOL CALLBACK chooseWindow(HWND window, LPARAM parameter) {
  auto& match = *reinterpret_cast<GameMatch*>(parameter);
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  if (pid != match.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
  RECT rect{}; GetClientRect(window, &rect);
  const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
  if (area > match.area) { match.area = area; match.window = window; }
  return TRUE;
}
static GameMatch findGame(const std::wstring& expected, bool diagnostic = false) {
  GameMatch found;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) { found.scanError = GetLastError(); return found; }
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) do {
    if (_wcsicmp(entry.szExeFile, L"BGI.exe") != 0) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
    if (!process) { if (GetLastError() == ERROR_ACCESS_DENIED) found.denied = true; continue; }
    wchar_t filename[32768]; DWORD size = 32768;
    if (QueryFullProcessImageNameW(process, 0, filename, &size)) {
      const bool same = _wcsicmp(canonicalPath(filename).c_str(), expected.c_str()) == 0;
      if (diagnostic) emit("{\"type\":\"game-candidate\",\"pid\":" + std::to_string(entry.th32ProcessID) + ",\"path\":" + quote(utf8(filename)) + ",\"matches\":" + (same ? "true" : "false") + "}");
      if (same) {
        GameMatch candidate; candidate.pid = entry.th32ProcessID;
        EnumWindows(chooseWindow, reinterpret_cast<LPARAM>(&candidate));
        // A Steam bootstrap process without a window must not hide a second,
        // matching BGI process that already owns the actual game window.
        if (!found.pid || candidate.area > found.area) {
          found.pid = candidate.pid; found.window = candidate.window; found.area = candidate.area;
        }
      }
    } else if (GetLastError() == ERROR_ACCESS_DENIED) found.denied = true;
    CloseHandle(process);
  } while (Process32NextW(snapshot, &entry));
  CloseHandle(snapshot); return found;
}
static bool steamInstallation(const std::wstring& game) {
  auto lower = game;
  CharLowerBuffW(lower.data(), lower.size());
  const std::wstring marker = L"\\steamapps\\common\\";
  const auto position = lower.find(marker);
  if (position == std::wstring::npos) return false;
  const auto manifest = game.substr(0, position) + L"\\steamapps\\appmanifest_251870.acf";
  const DWORD attributes = GetFileAttributesW(manifest.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
static bool launchGame(const std::wstring& game) {
  if (steamInstallation(game)) {
    // This exact executable belongs to Steam app 251870. Let Steam establish
    // its launch context and handle the bootstrap instead of starting the
    // wrapped executable directly.
    status("Launching Go! Go! Nippon! through Steam…", "Choose the 2011 edition if Steam asks which version to launch.");
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"steam://rungameid/251870", nullptr, nullptr, SW_SHOWNORMAL));
    if (result > 32) return true;
    status("Steam could not launch the game.", "Steam launch code: " + std::to_string(result) + ". Open the game through Steam, then reconnect.");
    return false;
  }
  status("Starting BGI.exe…", utf8(game));
  STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
  std::wstring command = L"\"" + game + L"\"";
  const auto parent = game.substr(0, game.find_last_of(L"\\/"));
  if (!CreateProcessW(game.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, parent.c_str(), &startup, &process)) {
    status("The game could not be started.", errorMessage()); return false;
  }
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return true;
}
static void snapshotCode() {
  HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, targetPid);
  if (modules == INVALID_HANDLE_VALUE) { status("Snapshot failed.", errorMessage()); return; }
  MODULEENTRY32W module{}; module.dwSize = sizeof(module);
  if (!Module32FirstW(modules, &module)) { CloseHandle(modules); return; }
  auto* base = module.modBaseAddr;
  std::vector<unsigned char> header(4096); SIZE_T actual;
  if (!ReadProcessMemory(targetProcess, base, header.data(), header.size(), &actual) || actual < sizeof(IMAGE_DOS_HEADER)) { CloseHandle(modules); return; }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(header.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > actual) { CloseHandle(modules); return; }
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(header.data() + dos->e_lfanew);
  const auto* sections = IMAGE_FIRST_SECTION(nt);
  const std::wstring stamp = std::to_wstring(GetTickCount64());
  std::string metadata = "{\"imageBase\":" + std::to_string(reinterpret_cast<uintptr_t>(base)) + ",\"sections\":[";
  bool first = true;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections && i < 32; ++i) {
    if (reinterpret_cast<const unsigned char*>(&sections[i + 1]) > header.data() + actual) break;
    if (!(sections[i].Characteristics & IMAGE_SCN_CNT_CODE)) continue;
    const DWORD size = sections[i].Misc.VirtualSize;
    if (!size || size > 16 * 1024 * 1024) continue;
    std::vector<unsigned char> bytes(size);
    if (!ReadProcessMemory(targetProcess, base + sections[i].VirtualAddress, bytes.data(), size, &actual)) continue;
    const std::wstring filename = L"code-" + stamp + L"-" + std::to_wstring(i) + L".bin";
    HANDLE file = CreateFileW((directory + L"\\" + filename).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, bytes.data(), static_cast<DWORD>(actual), &written, nullptr); CloseHandle(file); }
    if (!first) metadata += ',';
    first = false;
    metadata += "{\"file\":" + quote(utf8(filename)) + ",\"rva\":" + std::to_string(sections[i].VirtualAddress) + ",\"size\":" + std::to_string(actual) + "}";
  }
  metadata += "]}";
  HANDLE file = CreateFileW((directory + L"\\code-" + stamp + L".json").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, metadata.data(), metadata.size(), &written, nullptr); CloseHandle(file); }
  CloseHandle(modules);
  emit("{\"type\":\"snapshot\",\"metadata\":" + metadata + "}");
}
static bool inject(const std::wstring& dll, const StudyInit& init) {
  const size_t size = (dll.size() + 1) * sizeof(wchar_t);
  void* remoteName = VirtualAllocEx(targetProcess, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remoteName || !WriteProcessMemory(targetProcess, remoteName, dll.c_str(), size, nullptr)) return false;
  auto loader = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
  HANDLE thread = CreateRemoteThread(targetProcess, nullptr, 0, loader, remoteName, 0, nullptr);
  if (!thread) { VirtualFreeEx(targetProcess, remoteName, 0, MEM_RELEASE); return false; }
  DWORD wait = WaitForSingleObject(thread, 15000), moduleAddress = 0;
  if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &moduleAddress);
  CloseHandle(thread);
  // A timed-out remote thread may still read its argument; do not free it early.
  if (wait != WAIT_OBJECT_0) { SetLastError(ERROR_TIMEOUT); return false; }
  VirtualFreeEx(targetProcess, remoteName, 0, MEM_RELEASE);
  if (!moduleAddress) { SetLastError(ERROR_DLL_INIT_FAILED); return false; }
  HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!local) return false;
  auto entry = GetProcAddress(local, "StudyStart");
  if (!entry) { FreeLibrary(local); return false; }
  const uintptr_t entryRva = reinterpret_cast<uintptr_t>(entry) - reinterpret_cast<uintptr_t>(local);
  FreeLibrary(local);
  void* remoteInit = VirtualAllocEx(targetProcess, nullptr, sizeof(init), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remoteInit || !WriteProcessMemory(targetProcess, remoteInit, &init, sizeof(init), nullptr)) return false;
  thread = CreateRemoteThread(targetProcess, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(moduleAddress + entryRva), remoteInit, 0, nullptr);
  if (!thread) { VirtualFreeEx(targetProcess, remoteInit, 0, MEM_RELEASE); return false; }
  wait = WaitForSingleObject(thread, 10000);
  DWORD result = 0;
  if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &result);
  emit("{\"type\":\"hook-start-result\",\"waitResult\":" + std::to_string(wait) + ",\"exitCode\":" + std::to_string(result) + "}");
  CloseHandle(thread);
  if (wait == WAIT_OBJECT_0) VirtualFreeEx(targetProcess, remoteInit, 0, MEM_RELEASE);
  if (wait != WAIT_OBJECT_0) SetLastError(ERROR_TIMEOUT);
  else if ((result & 0xffff0000) == studyStartError) SetLastError(result & 0xffff);
  else if (!result) SetLastError(ERROR_ALREADY_EXISTS); // Older resident DLL.
  else if (result != 1) SetLastError(ERROR_DLL_INIT_FAILED);
  return wait == WAIT_OBJECT_0 && result == 1;
}
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  auto dpi = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (dpi) dpi(reinterpret_cast<HANDLE>(-4)); else SetProcessDPIAware();
  std::wstring game, dll, profile;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring key = argv[i];
    if (key == L"--game") game = argv[i + 1];
    else if (key == L"--dll") dll = argv[i + 1];
    else if (key == L"--session") directory = argv[i + 1];
    else if (key == L"--profile") profile = argv[i + 1];
  }
  if (game.empty() || dll.empty() || directory.empty() || game.size() >= 32700 || dll.size() >= 1024 || directory.size() >= 1024 || profile.size() >= 1024) {
    status("Invalid helper arguments."); return 2;
  }
  const std::wstring launchPath = game;
  game = canonicalPath(game);
  status("Looking for the game process…", utf8(game));
  std::thread([] {
    std::string command;
    while (std::getline(std::cin, command)) {
      if (command == "quit") { done = true; break; }
      if (command == "snapshot") snapshotRequested = true;
    }
    done = true;
  }).detach();
  auto match = findGame(game, true);
  if (done) return 0;
  if (match.scanError) { status("The game process list could not be read.", errorMessage(match.scanError)); return 5; }
  if (!match.pid && match.denied) {
    status("Access to BGI.exe was denied.", "Run the launcher and game with matching permissions, then reconnect."); return 5;
  }
  if (!match.pid) {
    if (!launchGame(launchPath)) return 3;
  } else {
    status("BGI.exe is already running; connecting…", "Connecting to the existing game.");
  }
  // Steam may replace the bootstrap process. Resolve by canonical image path.
  const ULONGLONG deadline = GetTickCount64() + 90000;
  ULONGLONG nextProgress = GetTickCount64() + 3000;
  while (!done && GetTickCount64() < deadline) {
    match = findGame(game);
    targetPid = match.pid; targetWindow = match.window;
    if (targetWindow && match.area > 64000) break;
    if (GetTickCount64() >= nextProgress) {
      const auto now = GetTickCount64();
      const auto seconds = now < deadline ? (deadline - now) / 1000 : 0;
      status(targetPid ? "BGI.exe is running; waiting for its window…" : "Waiting for the game to open…",
        "If Steam asks, choose Go! Go! Nippon! (2011). Time remaining: " + std::to_string(seconds) + " seconds.");
      nextProgress = GetTickCount64() + 5000;
    }
    Sleep(250);
  }
  if (done) return 0;
  if (!targetPid || !targetWindow) { status("The game window was not found.", "Open the game through Steam, wait for its title screen, then reconnect."); return 4; }
  if (IsIconic(targetWindow)) ShowWindowAsync(targetWindow, SW_RESTORE);
  SetForegroundWindow(targetWindow);
  emit("{\"type\":\"game-connected\",\"pid\":" + std::to_string(targetPid) + ",\"path\":" + quote(utf8(game)) + "}");
  targetProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE, FALSE, targetPid);
  if (!targetProcess) { status("The game process could not be accessed.", errorMessage()); return 5; }
  const std::wstring pipeName = L"\\\\.\\pipe\\gogonippon-study-" + std::to_wstring(targetPid) + L"-" + std::to_wstring(GetCurrentProcessId());
  EnginePipe pipe;
  if (!pipe.open(pipeName)) { status("The integration pipe could not be created.", errorMessage()); CloseHandle(targetProcess); return 6; }
  StudyInit init{}; init.size = sizeof(init);
  wcsncpy(init.pipe, pipeName.c_str(), 127); wcsncpy(init.directory, directory.c_str(), 1023); wcsncpy(init.profile, profile.c_str(), 1023);
  if (!inject(dll, init)) {
    const auto error = GetLastError();
    status("The engine hook could not be loaded.", error == ERROR_ALREADY_EXISTS ? "The game is connected to another launcher. Close that launcher or restart the game, then reconnect." : errorMessage(error));
    CloseHandle(targetProcess); return 7;
  }
  status("The game is connected; waiting for the engine adapter…", "The engine connection must send its startup acknowledgement before hover can become active.");
  snapshotCode();
  bool receivedEngineData = false, adapterReady = false, communicationFailed = false, automaticSnapshot = false;
  ULONGLONG lastSnapshot = GetTickCount64(), startupDeadline = GetTickCount64() + 10000;
  RECT previousGeometry{};
  std::string pending;
  while (!done && WaitForSingleObject(targetProcess, 0) == WAIT_TIMEOUT) {
    std::string chunk; DWORD pipeError = ERROR_SUCCESS;
    if (!pipe.poll(chunk, pipeError)) {
      emit("{\"type\":\"engine-pipe-error\",\"code\":" + std::to_string(pipeError) + "}");
      status("The engine connection closed.", errorMessage(pipeError) + " Close the game and launcher, reopen them, and export diagnostics if this repeats.");
      communicationFailed = true; break;
    }
    if (!chunk.empty()) {
      receivedEngineData = true;
      pending += chunk;
      size_t end;
      while ((end = pending.find('\n')) != std::string::npos) {
        const auto line = pending.substr(0, end);
        if (line.rfind("{\"type\":\"adapter-ready\",", 0) == 0) adapterReady = true;
        emit(line); pending.erase(0, end + 1);
      }
      if (pending.size() > 1024 * 1024) pending.clear();
    }
    if (!receivedEngineData && GetTickCount64() > startupDeadline) {
      status("The engine adapter did not acknowledge startup.", "The window was found, but the hook sent no data. Close both the game and launcher before reconnecting, then export diagnostics if this repeats.");
      communicationFailed = true; break;
    }
    if (!adapterReady && GetTickCount64() > startupDeadline + 20000) {
      status("The text hook did not finish initialization.", "The game window is connected, but the adapter did not become ready within 30 seconds. Export diagnostics to inspect the startup stage.");
      communicationFailed = true; break;
    }
    if (!IsWindow(targetWindow)) {
      GameMatch replacement; replacement.pid = targetPid;
      EnumWindows(chooseWindow, reinterpret_cast<LPARAM>(&replacement)); targetWindow = replacement.window;
    }
    RECT client{}; POINT origin{}, cursor{};
    GetClientRect(targetWindow, &client); ClientToScreen(targetWindow, &origin); GetCursorPos(&cursor);
    const RECT geometry{origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
    if (!EqualRect(&geometry, &previousGeometry)) {
      previousGeometry = geometry;
      emit("{\"type\":\"window-geometry\",\"probeVersion\":2,\"left\":" + std::to_string(origin.x) + ",\"top\":" + std::to_string(origin.y) +
        ",\"width\":" + std::to_string(client.right) + ",\"height\":" + std::to_string(client.bottom) + ",\"coordinateSpace\":\"physical-client\"}");
    }
    DWORD activePid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &activePid);
    emit("{\"type\":\"frame\",\"left\":" + std::to_string(origin.x) + ",\"top\":" + std::to_string(origin.y) +
      ",\"width\":" + std::to_string(client.right) + ",\"height\":" + std::to_string(client.bottom) +
      ",\"cursorX\":" + std::to_string(cursor.x) + ",\"cursorY\":" + std::to_string(cursor.y) +
      ",\"active\":" + (activePid == targetPid ? "true" : "false") +
      ",\"visible\":" + (IsWindowVisible(targetWindow) && !IsIconic(targetWindow) ? "true" : "false") +
      ",\"shift\":" + ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? "true" : "false") +
      ",\"escape\":" + ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? "true" : "false") + "}");
    if (snapshotRequested.exchange(false) || (!automaticSnapshot && GetTickCount64() - lastSnapshot > 15000)) {
      snapshotCode(); automaticSnapshot = true;
    }
    Sleep(25);
  }
  // Closing the server wakes the DLL's pipe checks so it disables its hooks.
  pipe.close(); Sleep(200);
  CloseHandle(targetProcess); return communicationFailed ? 8 : 0;
}
