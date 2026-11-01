// NG2 DIAG: trigger-file control for the whole-program function counter.
#include "ng2_trace_all.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <windows.h>

#include <rex/logging.h>

namespace ng2trace {
namespace {
std::wstring ExeDir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring s = buf;
  return s.substr(0, s.find_last_of(L"\/") + 1);
}
}  // namespace

void StartTriggerThread() {
  std::thread([] {
    const std::wstring dir = ExeDir();
    for (;;) {
      Sleep(50);
      if (GetFileAttributesW((dir + L"ng2_trace_on").c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileW((dir + L"ng2_trace_on").c_str());
        std::memset(g_counts, 0, sizeof(uint32_t) * Count());
        g_on = true;
        REXLOG_WARN("[trace] on");
      }
      WIN32_FIND_DATAW fd;
      HANDLE f = FindFirstFileW((dir + L"ng2_trace_off_*").c_str(), &fd);
      if (f == INVALID_HANDLE_VALUE) continue;
      std::wstring name = fd.cFileName;
      FindClose(f);
      DeleteFileW((dir + name).c_str());
      g_on = false;
      std::wstring tag = name.substr(std::wstring(L"ng2_trace_off_").size());
      FILE* out = _wfopen((dir + L"ng2_trace_" + tag + L".txt").c_str(), L"w");
      uint32_t n = 0;
      if (out) {
        for (uint32_t i = 0; i < Count(); ++i) {
          if (g_counts[i]) {
            std::fprintf(out, "%08X %u %08X\n", kAddrs[i], g_counts[i], g_lr[i]);
            ++n;
          }
        }
        std::fclose(out);
      }
      REXLOG_WARN("[trace] off, {} functions written", n);
    }
  }).detach();
}
}  // namespace ng2trace
