// openNG2 update check (see update_check.h).
#include "update_check.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace openng2 {
namespace {

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// HTTPS GET of a small document. False + error on failure.
bool HttpGet(const wchar_t* host, const wchar_t* path, const char* user_agent, std::string* body,
             std::string* error) {
  HINTERNET session = WinHttpOpen(Widen(user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *error = "WinHTTP is not available.";
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 15000);
  HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
  auto cleanup = [&] {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
  };
  const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
  if (!request || !WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    *error = "Could not reach GitHub (no internet connection?).";
    cleanup();
    return false;
  }
  DWORD status = 0, len = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                      nullptr);
  body->clear();
  char buf[16384];
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(request, buf, sizeof(buf), &got)) {
      *error = "The connection to GitHub was interrupted.";
      cleanup();
      return false;
    }
    if (!got) break;
    body->append(buf, got);
    if (body->size() > (8u << 20)) break;  // a release document is a few KB
  }
  cleanup();
  if (status == 404) {
    *error = "No openNG2 release is published yet.";
    return false;
  }
  if (status == 403 || status == 429) {
    *error = "GitHub is limiting update checks right now; try again later.";
    return false;
  }
  if (status != 200) {
    *error = "GitHub answered HTTP " + std::to_string(status) + ".";
    return false;
  }
  return true;
}

// Minimal JSON helpers for the GitHub release document (flat key lookups only).
size_t SkipWs(const std::string& j, size_t p) {
  while (p < j.size() && std::isspace(static_cast<unsigned char>(j[p]))) ++p;
  return p;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += char(cp);
  } else if (cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xF0 | (cp >> 18));
    out += char(0x80 | ((cp >> 12) & 0x3F));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

// Reads the JSON string starting at the quote at `p`; returns the position after it.
size_t ReadString(const std::string& j, size_t p, std::string* out) {
  out->clear();
  if (p >= j.size() || j[p] != '"') return std::string::npos;
  for (++p; p < j.size(); ++p) {
    const char c = j[p];
    if (c == '"') return p + 1;
    if (c != '\\') {
      *out += c;
      continue;
    }
    if (++p >= j.size()) break;
    switch (j[p]) {
      case 'n': *out += '\n'; break;
      case 'r': break;
      case 't': *out += '\t'; break;
      case 'b': case 'f': break;
      case 'u': {
        if (p + 4 >= j.size()) return std::string::npos;
        uint32_t cp = uint32_t(std::strtoul(j.substr(p + 1, 4).c_str(), nullptr, 16));
        p += 4;
        // Surrogate pair.
        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < j.size() && j[p + 1] == '\\' && j[p + 2] == 'u') {
          const uint32_t lo = uint32_t(std::strtoul(j.substr(p + 3, 4).c_str(), nullptr, 16));
          if (lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            p += 6;
          }
        }
        AppendUtf8(*out, cp);
        break;
      }
      default: *out += j[p]; break;  // \" \\ \/
    }
  }
  return std::string::npos;
}

// Finds `"key":` at or after `from` (and before `until`), returns the value's start or npos.
size_t FindKey(const std::string& j, std::string_view key, size_t from = 0, size_t until = std::string::npos) {
  const std::string needle = "\"" + std::string(key) + "\"";
  for (size_t p = j.find(needle, from); p != std::string::npos && p < until; p = j.find(needle, p + 1)) {
    const size_t colon = SkipWs(j, p + needle.size());
    if (colon < j.size() && j[colon] == ':') return SkipWs(j, colon + 1);
  }
  return std::string::npos;
}

std::string StringValue(const std::string& j, std::string_view key, size_t from = 0,
                        size_t until = std::string::npos) {
  std::string v;
  const size_t p = FindKey(j, key, from, until);
  if (p != std::string::npos) ReadString(j, p, &v);
  return v;
}

}  // namespace

int CompareVersions(std::string_view a, std::string_view b) {
  auto parts = [](std::string_view s) {
    std::vector<long> out;
    size_t i = 0;
    while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;  // "v1.0"
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      long n = 0;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + (s[i++] - '0');
      out.push_back(n);
      if (i < s.size() && s[i] == '.') ++i;
      else break;
    }
    while (!out.empty() && out.back() == 0) out.pop_back();  // "1.0" == "1"
    return out;
  };
  // No number at all ("dev"): older than any real version.
  const bool a_num = a.find_first_of("0123456789") != std::string_view::npos;
  const bool b_num = b.find_first_of("0123456789") != std::string_view::npos;
  if (a_num != b_num) return a_num ? 1 : -1;
  const auto pa = parts(a), pb = parts(b);
  for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
    const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
    if (x != y) return x < y ? -1 : 1;
  }
  return 0;
}

ReleaseInfo FetchLatestRelease(const char* user_agent) {
  ReleaseInfo r;
  std::string j;
  const std::wstring path = L"/repos/" + Widen(kRepo) + L"/releases/latest";
  if (!HttpGet(L"api.github.com", path.c_str(), user_agent, &j, &r.error)) return r;

  const std::string tag = StringValue(j, "tag_name");
  if (tag.empty()) {
    r.error = "GitHub's answer had no release version.";
    return r;
  }
  r.version = tag;
  while (!r.version.empty() && !std::isdigit(static_cast<unsigned char>(r.version[0]))) r.version.erase(0, 1);
  if (r.version.empty()) {
    r.error = "Unexpected release tag \"" + tag + "\".";
    return r;
  }
  r.page_url = "https://github.com/" + std::string(kRepo) + "/releases/tag/" + tag;
  r.notes = StringValue(j, "body");

  // Assets: each object ends with its browser_download_url; size and digest come earlier in
  // the same object, so search the stretch since the previous asset's URL.
  const size_t assets = FindKey(j, "assets");
  if (assets != std::string::npos) {
    size_t seg_start = assets;
    for (size_t p = FindKey(j, "browser_download_url", assets); p != std::string::npos;
         p = FindKey(j, "browser_download_url", p)) {
      std::string url;
      const size_t after = ReadString(j, p, &url);
      if (after == std::string::npos) break;
      const std::string name = StringValue(j, "name", seg_start, p);
      const bool is_zip = name.size() > 4 && name.rfind("openNG2", 0) == 0 &&
                          name.compare(name.size() - 4, 4, ".zip") == 0;
      if (is_zip) {
        r.zip_url = url;
        const size_t sz = FindKey(j, "size", seg_start, p);
        if (sz != std::string::npos) r.zip_size = std::strtoull(j.c_str() + sz, nullptr, 10);
        std::string digest = StringValue(j, "digest", seg_start, p);
        if (digest.rfind("sha256:", 0) == 0) {
          digest.erase(0, 7);
          for (auto& c : digest) c = char(std::toupper(static_cast<unsigned char>(c)));
          if (digest.size() == 64) r.zip_sha256 = digest;
        }
        break;
      }
      seg_start = after;
      p = after;
    }
  }
  r.ok = true;
  return r;
}

}  // namespace openng2
