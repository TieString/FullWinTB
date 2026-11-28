#include "motion_dialog.h"
#include "embedded_resource.h"
#include "resource.h"
#include "tray.h"

#include <objbase.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <shlwapi.h>
#include <wrl.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace fullwintb {
namespace {

using appearance::MotionSettings;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr wchar_t settingsUri[] = L"https://fullwintb.invalid/settings";
constexpr UINT editHotkey = WM_APP + 1;

std::string EscapeHtml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string Page(const TaskbarSettings& settings, int verticalDirection, bool enabled) {
    std::string html(EmbeddedResource(IDR_FEEL_PAGE));
    const auto use = [&](std::string_view token, std::string_view value) {
        const auto start = html.find(token);
        if (start != std::string::npos) html.replace(start, token.size(), value);
    };
    const auto& profile = settings.motion;
    char values[64]{};
    sprintf_s(values, "%d,%d,%d,%d,%d,%d,%u,%u,%u,%u", profile.mass, profile.damping, profile.click, profile.reach,
              profile.wheelStep, profile.hold, static_cast<UINT>(settings.wheelAxis), static_cast<UINT>(settings.ink),
              static_cast<UINT>(settings.returnStyle), static_cast<UINT>(settings.language));
    use("{{FEEL_CSS}}", EmbeddedResource(IDR_FEEL_CSS));
    use("{{FEEL_JS}}", EmbeddedResource(IDR_FEEL_RUNTIME));
    use("{{RIG_JS}}", EmbeddedResource(IDR_FEEL_RIG));
    use("{{PREVIEW_JS}}", EmbeddedResource(IDR_FEEL_PREVIEW));
    use("{{MOTION_SETTINGS}}", values);
    use("{{VERTICAL_DIRECTION}}", std::to_string(verticalDirection));
    use("{{RESET_DISABLED}}", enabled ? "" : "disabled");
    use("{{VERSION}}", FULLWINTB_VERSION);
    use("{{NOTICES}}", EscapeHtml(EmbeddedResource(IDR_THIRD_PARTY_NOTICES)));
    return html;
}

std::filesystem::path ProfileDirectory() {
    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(ARRAYSIZE(temporary), temporary);
    GUID id{};
    wchar_t suffix[40]{};
    if (!length || length >= ARRAYSIZE(temporary) || FAILED(CoCreateGuid(&id)) ||
        !StringFromGUID2(id, suffix, ARRAYSIZE(suffix))) return {};
    return std::filesystem::path(temporary) / (std::wstring(L"FullWinTBFeel-") + suffix);
}

struct ProfileCleanup {
    std::filesystem::path directory;
    HANDLE process = nullptr;
    ~ProfileCleanup() { if (process) CloseHandle(process); }
};

void RemoveProfile(std::filesystem::path directory, DWORD browserProcess) {
    if (directory.empty()) return;
    auto cleanup = std::make_unique<ProfileCleanup>();
    cleanup->directory = std::move(directory);
    if (browserProcess) cleanup->process = OpenProcess(SYNCHRONIZE, FALSE, browserProcess);
    if (TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* data) noexcept {
        const std::unique_ptr<ProfileCleanup> cleanup(static_cast<ProfileCleanup*>(data));
        if (cleanup->process && WaitForSingleObject(cleanup->process, 30000) != WAIT_OBJECT_0) return;
        // A renderer can release its cache handles just after the browser exits.
        for (int attempt = 0; attempt < 12; ++attempt) {
            std::error_code error;
            std::filesystem::remove_all(cleanup->directory, error);
            if (!error) break;
            Sleep(250);
        }
    }, cleanup.get(), nullptr)) cleanup.release();
}

int VerticalPreviewDirection() {
    RECT dock{};
    MONITORINFO monitor{sizeof(monitor)};
    const HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    return appearance::ReadDockBounds(bar, dock) &&
        GetMonitorInfoW(MonitorFromWindow(bar, MONITOR_DEFAULTTONULL), &monitor) &&
        dock.top == monitor.rcMonitor.top ? -1 : 1;
}

struct MotionView : std::enable_shared_from_this<MotionView> {
    MotionView(HWND app, Tray& tray, TaskbarSettings settings)
        : owner(app), settings(settings), tray(tray), directory(ProfileDirectory()) {}

    HWND owner;
    HWND window = nullptr;
    TaskbarSettings settings;
    Tray& tray;
    std::filesystem::path directory;
    HRESULT error = S_OK;
    bool closing = false;
    bool editingHotkey = false;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> page;
    std::optional<EventRegistrationToken> resourceRequest;

    void Finish(int result) {
        if (window && !std::exchange(closing, true)) PostMessageW(window, WM_COMMAND, result, 0);
    }

    void Failed(HRESULT result) {
        error = result;
        Finish(IDCANCEL);
    }

    void Resize() const {
        if (!controller) return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        controller->put_Bounds(bounds);
    }

    bool Enabled() const {
        return reinterpret_cast<UINT_PTR>(GetPropW(owner, appearance::requestedMode)) > 1;
    }

    void SendHotkey() const {
        if (page) page->PostWebMessageAsString(tray.HotkeyText(settings.language).c_str());
    }

    void UpdateTitle() const {
        SetWindowTextW(window, Localized(settings.language, L"FullWinTB · 设置", L"FullWinTB · Settings"));
    }

    void ReadMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
        LPWSTR rawSource = nullptr;
        if (FAILED(args->get_Source(&rawSource))) return;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> source(rawSource, CoTaskMemFree);
        if (!source || std::wstring_view(source.get()) != settingsUri || closing) return;
        LPWSTR rawMessage = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&rawMessage))) return;
        const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> message(rawMessage, CoTaskMemFree);
        if (!message) return;
        const std::wstring_view text(message.get());
        if (text == L"cancel") {
            Finish(IDCANCEL);
            return;
        }
        if (text == L"ready") return SendHotkey();
        if (text == L"hotkey") {
            if (!editingHotkey && PostMessageW(window, editHotkey, 0, 0)) editingHotkey = true;
            return;
        }
        if (text == L"reset") {
            if (Enabled()) appearance::ResetIcons(owner);
            return;
        }
        const bool save = text.starts_with(L"save ");
        if (text.size() > 52 || (!save && !text.starts_with(L"tune "))) return;
        MotionSettings received{};
        unsigned wheel = 0, ink = 0, style = 0, language = 0;
        wchar_t extra = L'\0';
        if (swscanf_s(message.get() + 5, L"%4d %5d %4d %3d %2d %4d %1u %1u %1u %1u %lc",
                      &received.mass, &received.damping, &received.click, &received.reach, &received.wheelStep,
                      &received.hold, &wheel, &ink, &style, &language, &extra, 1u) != 10 ||
            wheel > static_cast<UINT>(appearance::MotionAxis::vertical) ||
            ink > static_cast<UINT>(appearance::Ink::automatic) ||
            style > static_cast<UINT>(appearance::ReturnStyle::simple) ||
            language > static_cast<UINT>(UiLanguage::english) ||
            received != appearance::ClampMotionSettings(received)) return;
        if (settings.motion != received) {
            settings.motion = received;
            appearance::SetMotionSettings(owner, received);
        }
        const auto wheelAxis = static_cast<appearance::MotionAxis>(wheel);
        if (settings.wheelAxis != wheelAxis) {
            settings.wheelAxis = wheelAxis;
            appearance::SetWheelAxis(owner, wheelAxis);
        }
        const auto returnStyle = static_cast<appearance::ReturnStyle>(style);
        if (settings.returnStyle != returnStyle) {
            settings.returnStyle = returnStyle;
            appearance::SetReturnStyle(owner, returnStyle);
        }
        const auto selectedLanguage = static_cast<UiLanguage>(language);
        if (settings.language != selectedLanguage) {
            settings.language = selectedLanguage;
            UpdateTitle();
            SendHotkey();
        }
        settings.ink = static_cast<appearance::Ink>(ink);
        if (save) Finish(IDOK);
    }

    void OpenPage(ICoreWebView2Controller* created, const ComPtr<ICoreWebView2Environment>& environment) {
        controller = created;
        HRESULT result = controller->get_CoreWebView2(&page);
        if (FAILED(result)) return Failed(result);
        ComPtr<ICoreWebView2Settings> options;
        if (SUCCEEDED(page->get_Settings(&options))) {
            options->put_AreDefaultContextMenusEnabled(FALSE);
            options->put_AreDevToolsEnabled(FALSE);
            options->put_IsStatusBarEnabled(FALSE);
            options->put_AreHostObjectsAllowed(FALSE);
            ComPtr<ICoreWebView2Settings3> keys;
            if (SUCCEEDED(options.As(&keys))) keys->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        EventRegistrationToken token{};
        page->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                LPWSTR raw = nullptr;
                args->get_Uri(&raw);
                const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> uri(raw, CoTaskMemFree);
                if (!uri || std::wstring_view(uri.get()) != settingsUri) args->put_Cancel(TRUE);
                return S_OK;
            }).Get(), &token);
        page->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) { return args->put_Handled(TRUE); }
        ).Get(), &token);
        page->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
            [weak = weak_from_this()](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) {
                if (const auto view = weak.lock()) view->Failed(E_FAIL);
                return S_OK;
            }).Get(), &token);
        page->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [weak = weak_from_this()](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
                BOOL loaded = FALSE;
                if (SUCCEEDED(args->get_IsSuccess(&loaded)) && !loaded) {
                    if (const auto view = weak.lock()) view->Failed(E_FAIL);
                }
                return S_OK;
            }).Get(), &token);
        result = page->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak = weak_from_this()](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                if (const auto view = weak.lock()) view->ReadMessage(args);
                return S_OK;
            }).Get(), &token);
        if (FAILED(result)) return Failed(result);

        // Serve the embedded page from a fixed local origin. This also gives the
        // Feel export buttons a secure context for clipboard access.
        result = page->AddWebResourceRequestedFilter(settingsUri, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
        if (FAILED(result)) return Failed(result);
        auto html = Page(settings, VerticalPreviewDirection(), Enabled());
        if (html.empty()) return Failed(E_FAIL);
        result = page->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [environment, html = std::move(html)](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                ComPtr<IStream> content;
                content.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(html.data()), static_cast<UINT>(html.size())));
                if (!content) return E_OUTOFMEMORY;
                ComPtr<ICoreWebView2WebResourceResponse> response;
                const HRESULT status = environment->CreateWebResourceResponse(content.Get(), 200, L"OK",
                    L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store", &response);
                return SUCCEEDED(status) ? args->put_Response(response.Get()) : status;
            }).Get(), &token);
        if (FAILED(result)) return Failed(result);
        resourceRequest = token;
        Resize();
        result = page->Navigate(settingsUri);
        if (FAILED(result)) Failed(result);
    }

    void OpenController(ICoreWebView2Environment* environment) {
        auto ready = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [self = shared_from_this(), environment = ComPtr<ICoreWebView2Environment>(environment)]
            (HRESULT result, ICoreWebView2Controller* created) {
                if (!self->window) {
                    if (created) {
                        self->controller = created;
                        created->get_CoreWebView2(&self->page);
                    }
                    self->Close();
                } else if (FAILED(result) || !created) {
                    self->Failed(FAILED(result) ? result : E_FAIL);
                } else self->OpenPage(created, environment);
                return S_OK;
            });
        ComPtr<ICoreWebView2Environment10> profiles;
        ComPtr<ICoreWebView2ControllerOptions> options;
        const bool inPrivate = SUCCEEDED(environment->QueryInterface(IID_PPV_ARGS(&profiles))) &&
            SUCCEEDED(profiles->CreateCoreWebView2ControllerOptions(&options)) &&
            SUCCEEDED(options->put_IsInPrivateModeEnabled(TRUE));
        const HRESULT result = inPrivate ? profiles->CreateCoreWebView2ControllerWithOptions(window, options.Get(), ready.Get())
                                         : environment->CreateCoreWebView2Controller(window, ready.Get());
        if (FAILED(result)) Failed(result);
    }

    void Start() {
        if (directory.empty()) return Failed(E_FAIL);
        const auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (options) options->put_AdditionalBrowserArguments(L"--disable-gpu-shader-disk-cache");
        const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(nullptr, directory.c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self = shared_from_this()](HRESULT status, ICoreWebView2Environment* environment) {
                    if (!self->window) self->Close();
                    else if (FAILED(status) || !environment) self->Failed(FAILED(status) ? status : E_FAIL);
                    else self->OpenController(environment);
                    return S_OK;
                }).Get());
        if (FAILED(result)) Failed(result);
    }

    void Close() {
        UINT32 pid = 0;
        if (page) page->get_BrowserProcessId(&pid);
        if (page && resourceRequest) page->remove_WebResourceRequested(*resourceRequest);
        resourceRequest.reset();
        if (controller) controller->Close();
        page.Reset();
        controller.Reset();
        RemoveProfile(std::exchange(directory, {}), pid);
    }
};

void SizeToMonitor(HWND window) {
    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    const HMONITOR display = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(display, &monitor)) return;
    RECT work = monitor.rcWork;
    RECT dock{};
    if (appearance::ReadDockBounds(FindWindowW(L"Shell_TrayWnd", nullptr), dock) &&
        MonitorFromRect(&dock, MONITOR_DEFAULTTONULL) == display && dock.right - dock.left > dock.bottom - dock.top) {
        if (dock.top == monitor.rcMonitor.top) work.top = std::max(work.top, dock.bottom);
        if (dock.bottom == monitor.rcMonitor.bottom) work.bottom = std::min(work.bottom, dock.top);
    }
    const auto place = [&](UINT dpi) {
        const LONG margin = MulDiv(12, dpi, 96);
        const LONG width = std::max(1L, std::min<LONG>(MulDiv(720, dpi, 96), work.right - work.left - 2 * margin));
        const LONG height = std::max(1L, std::min<LONG>(MulDiv(540, dpi, 96), work.bottom - work.top - 2 * margin));
        SetWindowPos(window, nullptr, (work.left + work.right - width) / 2, (work.top + work.bottom - height) / 2,
                     width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    };
    const UINT dpi = GetDpiForWindow(window);
    place(dpi);
    if (const UINT movedDpi = GetDpiForWindow(window); movedDpi && movedDpi != dpi) place(movedDpi);
}

INT_PTR CALLBACK DialogProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        auto* view = reinterpret_cast<MotionView*>(lp);
        view->window = window;
        SetWindowLongPtrW(window, DWLP_USER, lp);
        view->UpdateTitle();
        SizeToMonitor(window);
        view->tray.SetWindowIcon(window);
        view->Start();
        return TRUE;
    }
    auto* view = reinterpret_cast<MotionView*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!view) return FALSE;
    if (message == WM_SIZE) view->Resize();
    if (message == editHotkey) {
        if (!view->closing) view->tray.ShowHotkeyDialog(window, view->settings.language);
        view->editingHotkey = false;
        view->SendHotkey();
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK && view->closing) {
        EndDialog(window, IDOK);
        return TRUE;
    }
    if (message == WM_DESTROY) {
        view->window = nullptr;
        if (view->controller || FAILED(view->error)) view->Close();
    }
    return FALSE;
}

} // namespace

std::optional<TaskbarSettings> EditTaskbarSettings(HWND owner, Tray& tray, TaskbarSettings settings) {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return std::nullopt;
    const auto view = std::make_shared<MotionView>(owner, tray, settings);
    const INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MOTION), owner,
                                           DialogProc, reinterpret_cast<LPARAM>(view.get()));
    if (result != IDOK) {
        if (view->settings.motion != settings.motion) appearance::SetMotionSettings(owner, settings.motion);
        if (view->settings.wheelAxis != settings.wheelAxis) appearance::SetWheelAxis(owner, settings.wheelAxis);
        if (view->settings.returnStyle != settings.returnStyle) appearance::SetReturnStyle(owner, settings.returnStyle);
    }
    if (FAILED(view->error)) {
        MessageBoxW(owner, Localized(settings.language, L"无法打开设置。请确认 Windows 已安装 Microsoft Edge WebView2 运行时。",
                    L"Could not open Settings. Make sure Microsoft Edge WebView2 Runtime is installed."),
                    L"FullWinTB", MB_OK | MB_ICONWARNING);
    }
    CoUninitialize();
    return result == IDOK ? std::optional(view->settings) : std::nullopt;
}

} // namespace fullwintb
