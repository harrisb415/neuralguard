// Small string / encoding / time helpers used across the core library.
// Header-only (inline) - trivial functions, not worth a translation unit.
#pragma once

#include <windows.h>
#include <cctype>
#include <cstdio>
#include <string>

namespace ng::util {

inline std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

inline std::string ToHex(const unsigned char* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string o; o.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { o += h[p[i] >> 4]; o += h[p[i] & 0xF]; }
    return o;
}

inline std::string IsoTime(const FILETIME& ft) {
    SYSTEMTIME st; FileTimeToSystemTime(&ft, &st);
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

inline std::string IsoNow() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return IsoTime(ft);
}

}  // namespace ng::util
