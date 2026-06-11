// win_util.hpp - small Win32 helpers shared by the WinHTTP engine.
//
// WinHTTP is a wide-character API (LPCWSTR for host, path, headers), but the
// rest of the program keeps configuration in UTF-8 std::string. `widen` is the
// single conversion point so the I/O core can stay readable.
#pragma once

#include <windows.h>

#include <stdexcept>
#include <string>

namespace ehlt {

// UTF-8 -> UTF-16. Throws on malformed input so a bad host/target fails loudly
// at startup rather than silently truncating a request line.
inline std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw std::runtime_error("MultiByteToWideChar failed");
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), n);
    return out;
}

}  // namespace ehlt
