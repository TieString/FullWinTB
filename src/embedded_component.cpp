#include "embedded_component.h"
#include "embedded_resource.h"
#include "resource.h"

#include <objbase.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>

namespace fullwintb {
namespace {

bool Matches(const std::filesystem::path& path, std::string_view bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool equal = GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(bytes.size());
    std::array<char, 32768> buffer;
    while (equal && !bytes.empty()) {
        const DWORD count = static_cast<DWORD>(std::min(bytes.size(), buffer.size()));
        DWORD read = 0;
        equal = ReadFile(file, buffer.data(), count, &read, nullptr) && read == count &&
                bytes.substr(0, count) == std::string_view(buffer.data(), count);
        bytes.remove_prefix(count);
    }
    CloseHandle(file);
    return equal;
}

bool Publish(const std::filesystem::path& path, std::string_view bytes) {
    GUID id{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, ARRAYSIZE(name))) return false;
    const auto staging = path.parent_path() / (std::wstring(name) + L".tmp");
    const HANDLE file = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD length = static_cast<DWORD>(bytes.size());
    DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(), length, &written, nullptr) && written == length &&
                          FlushFileBuffers(file);
    CloseHandle(file);
    if (complete && MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(staging.c_str());
    return Matches(path, bytes);
}

} // namespace

std::wstring PrepareAppearanceModule() noexcept try {
    const auto bytes = EmbeddedResource(IDR_APPEARANCE_MODULE);
    const auto hash = EmbeddedResource(IDR_APPEARANCE_HASH);
    if (bytes.empty() || hash.size() != 64 || hash.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        return {};

    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory))) return {};
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> localAppData(directory, CoTaskMemFree);
    const auto cache = std::filesystem::path(directory) / L"FullWinTB" / L"Components";
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    if (error) return {};

    // Explorer can keep an old image mapped after exit. Each build gets
    // its own path so publishing a new component never overwrites that image.
    const auto path = cache / (L"FullWinTBAppearance-" + std::wstring(hash.begin(), hash.end()) + L".dll");
    if (!Matches(path, bytes) && !Publish(path, bytes)) return {};
    return path.native();
} catch (...) {
    return {};
}

} // namespace fullwintb
