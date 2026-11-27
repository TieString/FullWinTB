#pragma once

#include "desktop_session.h"
#include "ink_sampling.h"
#include "work_area.h"
#include <array>
#include <vector>

namespace fullwintb {

class Overlay {
public:
    bool Start(HWND host);
    void Stop();
    void Refresh(bool retryWorkAreas = false);
    void ShellRestarted();
    void SessionLocked();
    void SessionUnlocked();
    void OnTimer(WPARAM timer);
    void OnAppBar(WPARAM notification);
    void SetInk(appearance::Ink ink);
    void UpdateAppearance();
    bool Enabled() const { return host_ != nullptr; }

    static constexpr UINT appBarMessage = WM_APP + 2;
    static constexpr UINT_PTR updateTimer = 1;
    static constexpr UINT_PTR settleTimer = 2;
    static constexpr UINT_PTR inkTimer = 3;
    static constexpr UINT_PTR resumeTimer = 4;

private:
    struct WindowId {
        HWND hwnd = nullptr;
        DWORD thread = 0;
        DWORD process = 0;

        explicit WindowId(HWND window);
        bool operator==(const WindowId&) const = default;
        bool Alive() const;
    };

    struct Taskbar {
        WindowId window;
        HMONITOR monitor;
        RECT bounds;
        MONITORINFOEXW display;
        Edge edge;
        bool primary;
        WorkArea work{};
        WindowId resizeAttempt{nullptr};
        WindowId raisedOver{nullptr};
        WindowId cover{nullptr};
        WindowId lastMaximized{nullptr};
        bool wasTopmost = false;
        bool wasVisible = true;
        bool hiddenByHost = false;
        bool topmostBeforeCover = false;
        bool requestedWorkArea = false;
        HWND lastOrder = nullptr;
        ULONGLONG lastOrderAt = 0;
        UINT lastOrderFlags = 0;
    };

    static void CALLBACK WinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    void OnEvent(DWORD event, HWND window);
    void Queue(UINT delay = 38);
    void QueueResume();
    void WakeInk();
    void CancelInk();
    void QueueInk(UINT delay);
    void SampleInk();
    void ReadTaskbars();
    void SizeMaximized(HWND window);
    void RestoreWorkAreas();
    void FloatTaskbar();
    void SetPrimaryLayer(appearance::LayerMode mode);
    void HideSecondaryBar(Taskbar& bar);
    bool OrderTaskbar(Taskbar& bar, HWND insertAfter, UINT flags);
    void RestoreAfterCover(Taskbar& bar);
    void RestoreOrder();

    HWND host_ = nullptr;
    std::array<HWINEVENTHOOK, 6> hooks_{};
    UINT queuedDelay_ = 0;
    bool topologyDirty_ = false;
    bool appBarRegistered_ = false;
    bool autoHide_ = false;
    bool taskbarLayoutDirty_ = false;
    bool retryWorkAreas_ = false;
    bool resumeQueued_ = false;
    appearance::LayerMode primaryLayer_ = appearance::LayerMode::native;
    appearance::Ink ink_ = appearance::Ink::automatic;
    appearance::AutomaticInk autoInk_;
    DesktopSession session_;
    ULONGLONG inkDue_ = 0;
    ULONGLONG inkBurstUntil_ = 0;
    std::vector<Taskbar> bars_;
};

} // namespace fullwintb
