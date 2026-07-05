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

// FILETIME (100ns since 1601) -> Unix epoch seconds (fractional).
inline double UnixEpoch(const FILETIME& ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return (double)(u.QuadPart - 116444736000000000ULL) / 1.0e7;
}

// Parse an ISO-8601 "YYYY-MM-DDThh:mm:ss[...]" (our stored form) to Unix epoch
// seconds. Ignores fractional seconds and the trailing 'Z'. 0 on parse failure.
inline double EpochFromIso(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 6) return 0;
    SYSTEMTIME st{};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)se;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return UnixEpoch(ft);
}

}  // namespace ng::util
