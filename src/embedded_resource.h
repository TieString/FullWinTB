#pragma once

#include <string_view>
#include <windows.h>

namespace fullwintb {

inline std::string_view EmbeddedResource(int id) {
    const HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!info) return {};
    const HGLOBAL resource = LoadResource(nullptr, info);
    const auto* data = resource ? static_cast<const char*>(LockResource(resource)) : nullptr;
    return data ? std::string_view(data, SizeofResource(nullptr, info)) : std::string_view{};
}

} // namespace fullwintb
