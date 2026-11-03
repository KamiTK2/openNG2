// openNG2 launcher: small Windows helpers.
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <thread>

namespace launcher {
namespace fs = std::filesystem;

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string PathText(const fs::path& p) { return Narrow(p.wstring()); }

namespace {
struct Hasher {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  Hasher() {
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (alg) BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
  }
  ~Hasher() {
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
  }
  void Add(const void* d, size_t n) {
    if (hash) BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(d)), ULONG(n), 0);
  }
  std::string Hex() {
    uint8_t out[32] = {};
    if (hash) BCryptFinishHash(hash, out, sizeof(out), 0);
    static const char* k = "0123456789ABCDEF";
    std::string s;
    for (uint8_t b : out) {
      s.push_back(k[b >> 4]);
      s.push_back(k[b & 15]);
    }
    return s;
  }
};
}  // namespace

std::string Sha256(const void* data, size_t size) {
  Hasher h;
  h.Add(data, size);
  return h.Hex();
}

std::string Sha256File(const fs::path& file, const std::function<bool()>& cancel) {
  std::ifstream f(file, std::ios::binary);
  if (!f) return {};
  Hasher h;
  std::vector<char> buf(4u << 20);
  while (f) {
    if (cancel && cancel()) return {};
    f.read(buf.data(), std::streamsize(buf.size()));
    if (f.gcount() > 0) h.Add(buf.data(), size_t(f.gcount()));
  }
  return h.Hex();
}

namespace {
// Every child (and its children: cmake -> ninja -> clang) runs in one job that is killed when the
// launcher's last handle closes - i.e. when the launcher exits for any reason. Without it, a
// closed or crashed launcher left its compilers running and holding the build folder (ninja
// "failed recompaction: Permission denied" on the next build).
HANDLE ChildJob() {
  static HANDLE job = [] {
    HANDLE j = CreateJobObjectW(nullptr, nullptr);
    if (j) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li));
    }
    return j;
  }();
  return job;
}
}  // namespace

int RunProcess(const std::wstring& command_line, const fs::path& work_dir,
               const std::vector<std::wstring>& env,
               const std::function<void(const std::string&)>& on_line,
               const std::function<bool()>& cancel) {
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read_pipe = nullptr, write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return -1;
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  std::wstring env_block;
  for (const auto& e : env) {
    env_block += e;
    env_block.push_back(L'\0');
  }
  env_block.push_back(L'\0');
  PROCESS_INFORMATION pi{};
  std::wstring cmd = command_line;
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                 env.empty() ? nullptr : env_block.data(),
                                 work_dir.empty() ? nullptr : work_dir.c_str(), &si, &pi);
  CloseHandle(write_pipe);
  if (!ok) {
    CloseHandle(read_pipe);
    return -1;
  }
  // Into the job before it can start any children of its own, then let it run.
  if (HANDLE job = ChildJob()) AssignProcessToJobObject(job, pi.hProcess);
  ResumeThread(pi.hThread);
  // Reader thread: split the output into lines.
  std::thread reader([&] {
    std::string pending;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) && n) {
      pending.append(buf, n);
      size_t pos;
      while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
        if (pos) on_line(pending.substr(0, pos));
        pending.erase(0, pos + 1);
      }
    }
    if (!pending.empty()) on_line(pending);
  });
  bool cancelled = false;
  while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
    if (cancel && cancel()) {
      // Ninja / compilers run as children: end every process in the job (all levels, even ones
      // whose parent already exited), then wait until none is left so the build folder is free.
      if (HANDLE job = ChildJob()) {
        TerminateJobObject(job, 1);
        for (int i = 0; i < 100; ++i) {  // up to 10 s
          JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
          if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc, sizeof(acc), nullptr) ||
              acc.ActiveProcesses == 0)
            break;
          Sleep(100);
        }
        cancelled = true;
        WaitForSingleObject(pi.hProcess, 10000);
        break;
      }
      std::wstring kill = L"taskkill /T /F /PID " + std::to_wstring(pi.dwProcessId);
      STARTUPINFOW ks{};
      ks.cb = sizeof(ks);
      PROCESS_INFORMATION kp{};
      if (CreateProcessW(nullptr, kill.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &ks, &kp)) {
        WaitForSingleObject(kp.hProcess, 10000);
        CloseHandle(kp.hProcess);
        CloseHandle(kp.hThread);
      }
      cancelled = true;
      WaitForSingleObject(pi.hProcess, 10000);
      break;
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  reader.join();
  CloseHandle(read_pipe);
  return cancelled ? -1 : int(code);
}

bool Download(const std::string& url, const fs::path& out_file,
              const std::function<void(uint64_t, uint64_t)>& progress,
              const std::function<bool()>& cancel, std::string* error) {
  const std::wstring wurl = Widen(url);
  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    *error = "Bad download address: " + url;
    return false;
  }
  HINTERNET session = WinHttpOpen(L"openNG2-launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = "Could not start the download (WinHTTP).";
    return false;
  }
  HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                              : nullptr;
  auto cleanup = [&] {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
  };
  if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    *error = "Download failed (no connection?): " + url;
    cleanup();
    return false;
  }
  DWORD status = 0, len = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                      nullptr);
  if (status != 200) {
    *error = "Download failed (HTTP " + std::to_string(status) + "): " + url;
    cleanup();
    return false;
  }
  uint64_t total = 0;
  wchar_t clen[32];
  DWORD clen_size = sizeof(clen);
  if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, clen, &clen_size, nullptr)) {
    total = _wcstoui64(clen, nullptr, 10);
  }
  std::error_code ec;
  fs::create_directories(out_file.parent_path(), ec);
  fs::path part = out_file;
  part += L".part";
  std::ofstream o(part, std::ios::binary | std::ios::trunc);
  std::vector<char> buf(1u << 20);
  uint64_t done = 0;
  bool ok = true;
  for (;;) {
    if (cancel && cancel()) {
      *error = "Cancelled.";
      ok = false;
      break;
    }
    DWORD got = 0;
    if (!WinHttpReadData(request, buf.data(), DWORD(buf.size()), &got)) {
      *error = "Download interrupted: " + url;
      ok = false;
      break;
    }
    if (!got) break;
    o.write(buf.data(), got);
    done += got;
    progress(done, total);
  }
  o.close();
  cleanup();
  if (ok && total && done != total) {
    *error = "Download incomplete: " + url;
    ok = false;
  }
  if (!ok) {
    fs::remove(part, ec);
    return false;
  }
  fs::rename(part, out_file, ec);
  if (ec) {
    *error = "Could not save " + PathText(out_file);
    return false;
  }
  return true;
}

uint64_t FreeDiskBytes(const fs::path& path) {
  fs::path p = path;
  std::error_code ec;
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  ULARGE_INTEGER free_bytes{};
  if (!GetDiskFreeSpaceExW(p.empty() ? nullptr : p.c_str(), &free_bytes, nullptr, nullptr)) return 0;
  return free_bytes.QuadPart;
}

uint64_t TotalRamBytes() {
  MEMORYSTATUSEX m{sizeof(m)};
  GlobalMemoryStatusEx(&m);
  return m.ullTotalPhys;
}

unsigned CpuThreads() { return std::max(1u, std::thread::hardware_concurrency()); }

}  // namespace launcher
