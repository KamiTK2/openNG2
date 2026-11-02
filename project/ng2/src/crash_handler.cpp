// Development crash handler: on an unhandled host exception, write ng2_crash.dmp next to
// the exe and log a symbolized host stack (function + file:line from ng2.pdb). Recompiled
// functions are named sub_<guest address> and every generated line carries the original
// PPC instruction as a comment, so the stack maps straight back to guest code.
#include "crash_handler.h"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::wstring ExeDir() {
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s(path, n);
  return s.substr(0, s.find_last_of(L"\\/") + 1);
}

void WriteDump(EXCEPTION_POINTERS* ep) {
  std::wstring file = ExeDir() + L"ng2_crash.dmp";
  HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                    MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory |
                                  MiniDumpWithThreadInfo),
                    &mei, nullptr, nullptr);
  CloseHandle(h);
}

void LogStack(EXCEPTION_POINTERS* ep) {
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(proc, nullptr, TRUE);

  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC = {ctx.Rip, 0, AddrModeFlat};
  frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
  frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat};

  auto* rec = ep->ExceptionRecord;
  REXLOG_CRITICAL("[crash] exception 0x{:08X} at host 0x{:016X}", rec->ExceptionCode,
                  reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    REXLOG_CRITICAL("[crash]   {} of host 0x{:016X}",
                    rec->ExceptionInformation[0] == 1 ? "write" : "read",
                    rec->ExceptionInformation[1]);
  }

  alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 32; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        !frame.AddrPC.Offset) {
      break;
    }
    DWORD64 pc = frame.AddrPC.Offset;
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "?";
    IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
    DWORD ldisp = 0;
    std::string where = "";
    if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
      std::string f = line.FileName;
      where = f.substr(f.find_last_of("\\/") + 1) + ":" + std::to_string(line.LineNumber);
    }
    REXLOG_CRITICAL("[crash] #{:02} {}+0x{:X} {}", i, name, disp, where);
  }
  rex::FlushLogging();
}

constexpr int kMaxFrames = 24;

// Collect return addresses only (no allocation, no logging): the target thread is
// suspended and may hold the heap or logger lock.
int CollectStack(HANDLE thread, CONTEXT ctx, DWORD64 (&pcs)[kMaxFrames]) {
  HANDLE proc = GetCurrentProcess();
  STACKFRAME64 frame{};
  frame.AddrPC = {ctx.Rip, 0, AddrModeFlat};
  frame.AddrFrame = {ctx.Rbp, 0, AddrModeFlat};
  frame.AddrStack = {ctx.Rsp, 0, AddrModeFlat};
  int n = 0;
  while (n < kMaxFrames &&
         StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
         frame.AddrPC.Offset) {
    pcs[n++] = frame.AddrPC.Offset;
  }
  return n;
}

void LogPcs(const std::string& tag, const DWORD64* pcs, int n) {
  HANDLE proc = GetCurrentProcess();
  alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < n; ++i) {
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name = SymFromAddr(proc, pcs[i], &disp, sym) ? sym->Name : "?";
    IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
    DWORD ldisp = 0;
    std::string where;
    if (SymGetLineFromAddr64(proc, pcs[i], &ldisp, &line)) {
      std::string f = line.FileName;
      where = f.substr(f.find_last_of("\\/") + 1) + ":" + std::to_string(line.LineNumber);
    }
    REXLOG_WARN("[stacks] {} #{:02} {}+0x{:X} {}", tag, i, name, disp, where);
  }
}

// Guest memory is mapped at a fixed host base (see "Guest memory arena mapped").
constexpr uint64_t kGuestVirtualBase = 0x100000000ull;

// Copy guest memory without ever faulting: only committed, readable host pages are
// read (no-access / guard pages are left zeroed). Faulting here would go through the
// runtime's guest access-violation handler, which doesn't expect non-guest threads.
bool ReadGuest(uint32_t addr, void* out, size_t n) {
  auto* dst = static_cast<uint8_t*>(out);
  auto* src = reinterpret_cast<const uint8_t*>(kGuestVirtualBase + addr);
  bool all = true;
  size_t done = 0;
  while (done < n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(src + done, &mbi, sizeof(mbi))) {
      std::memset(dst + done, 0, n - done);
      return false;
    }
    size_t region_left =
        size_t(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - (src + done));
    size_t chunk = std::min(region_left, n - done);
    const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
    if (mbi.State == MEM_COMMIT && !(mbi.Protect & bad)) {
      std::memcpy(dst + done, src + done, chunk);
    } else {
      std::memset(dst + done, 0, chunk);
      all = false;
    }
    done += chunk;
  }
  return all;
}
uint32_t GuestU32(uint32_t addr) {
  uint32_t v = 0;
  ReadGuest(addr, &v, 4);
  return _byteswap_ulong(v);
}

// NG2: audio engine barrier state (sub_837746E0 / sub_83774818). Engine pointer is
// the global at 0x84C55A9C; per-CPU participation flags at +308..+328, participant
// count at +304, callback thread at +300, barrier slots (8 bytes) at +356 / +364.
void LogAudioEngineState() {
  uint32_t engine = GuestU32(0x84C55A9C);
  if (!engine) {
    REXLOG_WARN("[stacks] audio engine: null");
    return;
  }
  uint8_t slots[16] = {};
  ReadGuest(engine + 356, slots, 16);
  uint8_t passes = 0;
  ReadGuest(engine + 128, &passes, 1);
  REXLOG_WARN(
      "[stacks] audio engine {:08X}: cb_thread(+300) {:08X} count(+304) {} cpu flags(+308..+328) "
      "{} {} {} {} {} {} passes(+128) {}",
      engine, GuestU32(engine + 300), GuestU32(engine + 304), GuestU32(engine + 308),
      GuestU32(engine + 312), GuestU32(engine + 316), GuestU32(engine + 320),
      GuestU32(engine + 324), GuestU32(engine + 328), passes);
  REXLOG_WARN(
      "[stacks] audio barrier slotA(+356) {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} "
      "slotB(+364) {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
      slots[0], slots[1], slots[2], slots[3], slots[4], slots[5], slots[6], slots[7], slots[8],
      slots[9], slots[10], slots[11], slots[12], slots[13], slots[14], slots[15]);
}

// Suspend every other thread in turn and log its symbolized stack.
void DumpAllThreadStacks() {
  LogAudioEngineState();
  static bool sym_ready = false;
  if (!sym_ready) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    sym_ready = true;
  }
  DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, te.th32ThreadID);
    if (!t) continue;
    std::string tname;
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
      for (PWSTR c = desc; *c; ++c) tname.push_back(*c < 128 ? char(*c) : '?');
      LocalFree(desc);
    }
    std::string tag = "tid " + std::to_string(te.th32ThreadID) + " '" + tname + "'";
    DWORD64 pcs[kMaxFrames];
    int n = 0;
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &ctx)) {
        n = CollectStack(t, ctx, pcs);
      }
      ResumeThread(t);
    }
    LogPcs(tag, pcs, n);
    CloseHandle(t);
  }
  CloseHandle(snap);
  rex::FlushLogging();
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
  static std::once_flag once;
  std::call_once(once, [ep] {
    LogStack(ep);
    WriteDump(ep);
  });
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// Dump guest memory ranges to <exe>\memdump_<tag>.bin (raw, page-by-page; unreadable
// pages are written as zeros). Layout: for each range, 4-byte LE start, 4-byte LE size,
// then the bytes. Used for memory diffing (tools\memdiff.py).
void DumpGuestMemory(const std::wstring& tag) {
  struct Range { uint32_t start, size; };
  const Range ranges[] = {
      {0x82000000u, 0x03A90000u},  // XEX image incl. .data/.bss
      {0x40000000u, 0x10000000u},  // guest virtual heap (first 256 MB)
      {0xA0000000u, 0x20000000u},  // physical memory (512 MB) via 64K-page view
  };
  std::wstring path = ExeDir() + L"memdump_" + tag + L".bin";
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  std::vector<uint8_t> page(0x10000);
  DWORD written;
  for (const auto& r : ranges) {
    WriteFile(h, &r.start, 4, &written, nullptr);
    WriteFile(h, &r.size, 4, &written, nullptr);
    for (uint32_t off = 0; off < r.size; off += uint32_t(page.size())) {
      ReadGuest(r.start + off, page.data(), page.size());
      WriteFile(h, page.data(), DWORD(page.size()), &written, nullptr);
    }
  }
  CloseHandle(h);
  REXLOG_WARN("[memdump] wrote memdump_{}.bin", std::string(tag.begin(), tag.end()));
}

// ---- NG2 DIAG: hardware data watchpoints ----------------------------------------------
// Create ng2_watch.txt next to ng2.exe with up to 4 guest addresses (hex, one per line).
// Once the player is in gameplay, DR0-3 are set on every thread for 8 s (read/write, 4 bytes)
// and each accessing host instruction is logged with its symbol and generated line (which
// carries the PPC instruction), i.e. which recompiled function touches the data.
bool Ng2InGameplay();  // ng2_hooks.cpp

namespace {

std::atomic<int> g_f9_presses{0};  // F9 marker count (watch can wait for it)
bool g_watch_wait_f9 = false;

constexpr int kWatchSlots = 512;
std::atomic<uint64_t> g_watch_pc[kWatchSlots];
std::atomic<uint32_t> g_watch_count[kWatchSlots];
std::atomic<uint32_t> g_watch_which[kWatchSlots];
std::atomic<bool> g_watch_on{false};

LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch_on) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  CONTEXT* c = ep->ContextRecord;
  const uint32_t hit = uint32_t(c->Dr6 & 0xF);
  if (!hit) return EXCEPTION_CONTINUE_SEARCH;
  const uint64_t pc = c->Rip;
  for (int i = 0, h = int((pc >> 2) % kWatchSlots); i < kWatchSlots; ++i, h = (h + 1) % kWatchSlots) {
    uint64_t cur = g_watch_pc[h].load();
    if (cur == pc || (cur == 0 && g_watch_pc[h].compare_exchange_strong(cur, pc)) || cur == pc) {
      g_watch_count[h].fetch_add(1);
      g_watch_which[h].fetch_or(hit);
      break;
    }
  }
  c->Dr6 = 0;
  return EXCEPTION_CONTINUE_EXECUTION;
}

std::vector<uint32_t> g_watch_lens;  // 4 or 8 bytes per address

void SetWatchRegs(const std::vector<uint32_t>& guest_addrs) {
  DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{sizeof(te)};
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
                          te.th32ThreadID);
    if (!t) continue;
    if (SuspendThread(t) != DWORD(-1)) {
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(t, &ctx)) {
        DWORD64 dr7 = 0;
        DWORD64* regs[4] = {&ctx.Dr0, &ctx.Dr1, &ctx.Dr2, &ctx.Dr3};
        for (size_t i = 0; i < 4; ++i) {
          const bool eight = i < g_watch_lens.size() && g_watch_lens[i] == 8;
          *regs[i] = i < guest_addrs.size()
                         ? kGuestVirtualBase + (guest_addrs[i] & (eight ? ~7u : ~3u))
                         : 0;
          if (i < guest_addrs.size()) {
            dr7 |= 1ull << (2 * i);                          // local enable
            dr7 |= 3ull << (16 + 4 * i);                     // read/write
            dr7 |= (eight ? 2ull : 3ull) << (18 + 4 * i);    // 8 or 4 bytes
          }
        }
        ctx.Dr7 = dr7;
        ctx.Dr6 = 0;
        SetThreadContext(t, &ctx);
      }
      ResumeThread(t);
    }
    CloseHandle(t);
  }
  CloseHandle(snap);
}

void RunWatch(std::vector<uint32_t> addrs) {
  if (g_watch_wait_f9) {
    REXLOG_WARN("[watch] armed for {} address(es), waiting for F9", addrs.size());
    const int start = g_f9_presses.load();
    for (int i = 0; i < 6000 && g_f9_presses.load() == start; ++i) Sleep(20);
  } else {
    REXLOG_WARN("[watch] armed for {} address(es), waiting for gameplay", addrs.size());
    for (int i = 0; i < 6000 && !::Ng2InGameplay(); ++i) Sleep(100);
  }
  REXLOG_WARN("[watch] started");
  static bool veh = false;
  if (!veh) {
    AddVectoredExceptionHandler(1, WatchVeh);
    veh = true;
  }
  for (auto& p : g_watch_pc) p = 0;
  for (auto& c : g_watch_count) c = 0;
  for (auto& w : g_watch_which) w = 0;
  g_watch_on = true;
  for (int i = 0; i < 16; ++i) {  // 8 s; re-apply for threads created meanwhile
    SetWatchRegs(addrs);
    Sleep(500);
  }
  SetWatchRegs({});
  Sleep(100);
  g_watch_on = false;
  static bool sym_ready = false;
  if (!sym_ready) {
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    sym_ready = true;
  }
  int n = 0;
  for (int i = 0; i < kWatchSlots; ++i) {
    const uint64_t pc = g_watch_pc[i].load();
    if (!pc) continue;
    ++n;
    HANDLE proc = GetCurrentProcess();
    alignas(SYMBOL_INFO) char symbuf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    std::string name = SymFromAddr(proc, pc, &disp, sym) ? sym->Name : "?";
    IMAGEHLP_LINE64 line{sizeof(IMAGEHLP_LINE64)};
    DWORD ldisp = 0;
    std::string where;
    if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
      std::string f = line.FileName;
      where = f.substr(f.find_last_of("\\/") + 1) + ":" + std::to_string(line.LineNumber);
    }
    REXLOG_WARN("[watch] slots {:X} hits {} at {}+0x{:X} {}", g_watch_which[i].load(),
                g_watch_count[i].load(), name, disp, where);
  }
  REXLOG_WARN("[watch] done, {} accessing instruction(s)", n);
}

void InstallCrashHandler_Watch() {
  std::thread([] {
    const std::wstring path = ExeDir() + L"ng2_watch.txt";
    for (;;) {
      Sleep(1000);
      FILE* f = _wfopen(path.c_str(), L"r");
      if (!f) continue;
      std::vector<uint32_t> addrs;
      g_watch_lens.clear();
      g_watch_wait_f9 = false;
      int repeat = 1;
      char line[128];
      while (addrs.size() < 4 && fgets(line, sizeof(line), f)) {
        unsigned v = 0, len = 4;
        if (std::strncmp(line, "wait_f9", 7) == 0) {
          g_watch_wait_f9 = true;
          continue;
        }
        if (std::sscanf(line, "repeat %d", &repeat) == 1) continue;
        if (sscanf(line, "%x %u", &v, &len) >= 1) {
          addrs.push_back(v);
          g_watch_lens.push_back(len == 8 ? 8 : 4);
        }
      }
      fclose(f);
      DeleteFileW(path.c_str());
      for (int r = 0; r < std::max(1, repeat) && !addrs.empty(); ++r) RunWatch(addrs);
    }
  }).detach();
}

}  // namespace

// NG2 DIAG: grab a burst of frames from our own window (PrintWindow, full content), saved as
// downscaled BMPs next to ng2.exe: effect_<tag>_<n>.bmp. For effects too short to screenshot.
void Ng2CaptureBurst(const char* tag) {
  static std::atomic<int> bursts{0};
  static std::atomic<bool> busy{false};
  if (bursts.load() >= 6 || busy.exchange(true)) return;
  const int burst = ++bursts;
  std::string t = tag;
  std::thread([burst, t] {
    HWND hwnd = nullptr;
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
          DWORD pid = 0;
          GetWindowThreadProcessId(h, &pid);
          if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
            *reinterpret_cast<HWND*>(p) = h;
            return FALSE;
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&hwnd));
    if (hwnd) {
      RECT rc;
      GetClientRect(hwnd, &rc);
      const int w = rc.right, h = rc.bottom, ow = 1280, oh = std::max(1, h * 1280 / std::max(1, w));
      HDC wdc = GetDC(hwnd);
      for (int n = 0; n < 10; ++n) {
        HDC full = CreateCompatibleDC(wdc), small = CreateCompatibleDC(wdc);
        HBITMAP fb = CreateCompatibleBitmap(wdc, w, h), sb = CreateCompatibleBitmap(wdc, ow, oh);
        HGDIOBJ of = SelectObject(full, fb), os = SelectObject(small, sb);
        PrintWindow(hwnd, full, 3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */);
        SetStretchBltMode(small, HALFTONE);
        StretchBlt(small, 0, 0, ow, oh, full, 0, 0, w, h, SRCCOPY);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), ow, oh, 1, 24, BI_RGB};
        const int stride = (ow * 3 + 3) & ~3;
        std::vector<uint8_t> px(size_t(stride) * oh);
        GetDIBits(small, sb, 0, oh, px.data(), &bi, DIB_RGB_COLORS);
        const std::wstring path = ExeDir() + L"effect_" + std::wstring(t.begin(), t.end()) +
                                  L"_" + std::to_wstring(burst) + L"_" + std::to_wstring(n) + L".bmp";
        if (FILE* f = _wfopen(path.c_str(), L"wb")) {
          BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER) + px.size()), 0, 0,
                              DWORD(sizeof(fh) + sizeof(BITMAPINFOHEADER))};
          fwrite(&fh, sizeof(fh), 1, f);
          fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
          fwrite(px.data(), 1, px.size(), f);
          fclose(f);
        }
        SelectObject(full, of);
        SelectObject(small, os);
        DeleteObject(fb);
        DeleteObject(sb);
        DeleteDC(full);
        DeleteDC(small);
        Sleep(50);
      }
      ReleaseDC(hwnd, wdc);
      REXLOG_WARN("[capture] effect burst {} saved ({})", burst, t);
    }
    Sleep(3000);  // cooldown
    busy = false;
  }).detach();
}

void InstallCrashHandler() {
  InstallCrashHandler_Watch();
  SetUnhandledExceptionFilter(Filter);
  // On-demand guest memory dump: create "ng2_dumpmem_<tag>" next to ng2.exe.
  std::thread([] {
    int hotkey_count = 0;
    bool f8_was_down = false;
    for (;;) {
      Sleep(50);
      // NG2 DIAG: F8 (game window focused) writes memdump_cam<N>.bin, for memory diffs that
      // need the player to do something between snapshots (e.g. turn the camera).
      {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const bool f8 = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (f8 && !f8_was_down) {
          DumpGuestMemory(L"cam" + std::to_wstring(++hotkey_count));
        }
        f8_was_down = f8;
        // F9: a marker line in the log (to line diagnostics up with what the player did).
        static bool f9_was_down = false;
        static int marks = 0;
        const bool f9 = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000);
        if (f9 && !f9_was_down) {
          REXLOG_WARN("[mark] MARK {}", ++marks);
          g_f9_presses.fetch_add(1);
        }
        f9_was_down = f9;
      }
      WIN32_FIND_DATAW fd;
      std::wstring pattern = ExeDir() + L"ng2_dumpmem_*";
      HANDLE f = FindFirstFileW(pattern.c_str(), &fd);
      if (f == INVALID_HANDLE_VALUE) continue;
      std::wstring name = fd.cFileName;
      FindClose(f);
      DeleteFileW((ExeDir() + name).c_str());
      DumpGuestMemory(name.substr(std::wstring(L"ng2_dumpmem_").size()));
    }
  }).detach();
  // NG2 DIAG (cvar ng2_thread_cpu): every 5 s, CPU use per thread (% of one core), by name.
  std::thread([] {
    std::unordered_map<DWORD, uint64_t> last;
    for (;;) {
      Sleep(5000);
      if (rex::cvar::GetFlagByName("ng2_thread_cpu") != "true") continue;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap == INVALID_HANDLE_VALUE) continue;
      struct Row { double pct; std::string name; DWORD tid; };
      std::vector<Row> rows;
      double total = 0;
      std::unordered_map<DWORD, uint64_t> now;
      THREADENTRY32 te{sizeof(te)};
      const DWORD pid = GetCurrentProcessId();
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(t, &c, &e, &k, &u)) {
          const uint64_t busy = ((uint64_t(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
                                ((uint64_t(u.dwHighDateTime) << 32) | u.dwLowDateTime);
          now[te.th32ThreadID] = busy;
          auto it = last.find(te.th32ThreadID);
          if (it != last.end() && busy >= it->second) {
            const double pct = double(busy - it->second) / 5e7 * 100.0;  // 100 ns units / 5 s
            total += pct;
            std::string name;
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(t, &desc)) && desc) {
              for (PWSTR ch = desc; *ch; ++ch) name.push_back(*ch < 128 ? char(*ch) : '?');
              LocalFree(desc);
            }
            if (pct >= 1.0) rows.push_back({pct, name, te.th32ThreadID});
          }
        }
        CloseHandle(t);
      }
      CloseHandle(snap);
      last = std::move(now);
      std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
      std::string line;
      for (size_t i = 0; i < rows.size() && i < 14; ++i) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " [%.0f%% %s #%lu]", rows[i].pct,
                      rows[i].name.empty() ? "?" : rows[i].name.c_str(), rows[i].tid);
        line += buf;
      }
      REXLOG_INFO("NG2 thread CPU 5s: total {:.0f}% of one core;{}", total, line);
    }
  }).detach();
  // On-demand stack dump: create a file named "ng2_dumpstacks" next to ng2.exe and
  // every thread's symbolized stack is written to the log (the file is then deleted).
  std::thread([] {
    std::wstring trigger = ExeDir() + L"ng2_dumpstacks";
    for (;;) {
      Sleep(1000);
      if (GetFileAttributesW(trigger.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(trigger.c_str());
        REXLOG_WARN("[stacks] dump requested");
        DumpAllThreadStacks();
      }
    }
  }).detach();
}

#else
void InstallCrashHandler() {}
#endif


