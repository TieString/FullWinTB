#pragma once

namespace fullwintb {

enum class UiLanguage { chinese, english };

constexpr const wchar_t* Localized(UiLanguage language, const wchar_t* chinese, const wchar_t* english) {
    return language == UiLanguage::english ? english : chinese;
}

} // namespace fullwintb
