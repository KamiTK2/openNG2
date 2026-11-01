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
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

void InstallCrashHandler() {
  SetUnhandledExceptionFilter(Filter);
  // On-demand guest memory dump: create "ng2_dumpmem_<tag>" next to ng2.exe.
  std::thread([] {
    for (;;) {
      Sleep(200);
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


