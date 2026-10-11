#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace PrismForge {
inline std::wstring ApplicationDirectory() {
    std::vector<wchar_t> buffer(256);
    while (buffer.size() <= 32768) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!size) return {};
        if (size < buffer.size()) {
            const std::wstring path(buffer.data(), size);
            const auto separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring{} : path.substr(0, separator);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}
}
