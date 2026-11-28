#include "taskbar_layout.h"
#include "geometry.h"

#include <unknwn.h>
#include <UIAutomation.h>
#include <winrt/base.h>
#include <string_view>

namespace fullwintb::appearance {
namespace {

void MergeButtonSpans(TaskbarLayout& layout) {
    auto& buttons = layout.buttons;
    const auto axis = layout.Axis();
    const LONG padding = std::max(2, MulDiv(3, layout.dpi, 96));
    std::sort(buttons.begin(), buttons.end(), [axis](const RECT& first, const RECT& second) {
        return AxisStart(first, axis) < AxisStart(second, axis);
    });
    std::size_t used = 0;
    for (const auto& bounds : buttons) {
        RECT strip = layout.dock;
        AxisStart(strip, axis) = std::max(AxisStart(layout.dock, axis), AxisStart(bounds, axis) - padding);
        AxisEnd(strip, axis) = std::min(AxisEnd(layout.dock, axis), AxisEnd(bounds, axis) + padding);
        if (used && AxisStart(strip, axis) <= AxisEnd(buttons[used - 1], axis)) {
            auto& last = buttons[used - 1];
            AxisEnd(last, axis) = std::max(AxisEnd(last, axis), AxisEnd(strip, axis));
        } else {
            buttons[used++] = strip;
        }
    }
    buttons.resize(used);
}

} // namespace

TaskbarLayout ReadTaskbarGeometry(HWND bar) {
    TaskbarLayout result;
    result.bar = bar;
    result.dpi = GetDpiForWindow(bar);
    const HWND notification = FindWindowExW(bar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!result.dpi || !notification || !GetWindowRect(bar, &result.bounds) ||
        !ReadDockBounds(bar, result.dock) ||
        !GetWindowRect(notification, &result.notification) ||
        !Overlaps(result.bounds, result.notification)) return {};
    const auto across = OtherAxis(result.Axis());
    AxisStart(result.notification, across) = AxisStart(result.dock, across);
    AxisEnd(result.notification, across) = AxisEnd(result.dock, across);
    result.monitor = result.dock;
    MONITORINFO monitor{sizeof(monitor)};
    if (GetMonitorInfoW(MonitorFromRect(&result.dock, MONITOR_DEFAULTTONEAREST), &monitor))
        result.monitor = monitor.rcMonitor;
    constexpr wchar_t advanced[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    DWORD widgets = 1, bytes = sizeof(widgets);
    RegGetValueW(HKEY_CURRENT_USER, advanced, L"TaskbarDa", RRF_RT_REG_DWORD, nullptr, &widgets, &bytes);
    result.widgets = widgets != 0;
    return result;
}

namespace {

TaskbarLayout Measure(HWND bar, IconOffset offset) {
    auto result = ReadTaskbarGeometry(bar);
    if (!result.bar) return {};
    winrt::com_ptr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(automation.put())))) return {};
    if (const auto timeouts = automation.try_as<IUIAutomation2>()) {
        timeouts->put_ConnectionTimeout(600);
        timeouts->put_TransactionTimeout(850);
    }

    winrt::com_ptr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(bar, root.put())) || !root) return {};

    VARIANT id{};
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"TaskbarFrame");
    if (!id.bstrVal) return {};
    winrt::com_ptr<IUIAutomationCondition> condition;
    const HRESULT conditionResult = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,
                                                                        id, condition.put());
    VariantClear(&id);
    if (FAILED(conditionResult)) return {};
    winrt::com_ptr<IUIAutomationElement> frame;
    if (FAILED(root->FindFirst(TreeScope_Descendants, condition.get(), frame.put())) || !frame)
        return {};

    winrt::com_ptr<IUIAutomationCacheRequest> cache;
    if (FAILED(automation->CreateCacheRequest(cache.put())) ||
        FAILED(cache->AddProperty(UIA_ControlTypePropertyId)) ||
        FAILED(cache->AddProperty(UIA_AutomationIdPropertyId)) ||
        FAILED(cache->AddProperty(UIA_BoundingRectanglePropertyId))) return {};
    winrt::com_ptr<IUIAutomationCondition> any;
    if (FAILED(automation->CreateTrueCondition(any.put()))) return {};
    winrt::com_ptr<IUIAutomationElementArray> items;
    if (FAILED(frame->FindAllBuildCache(TreeScope_Children, any.get(), cache.get(), items.put())) ||
        !items) return {};

    int count = 0;
    if (FAILED(items->get_Length(&count)) || count > 256) return {};
    RECT nativeStart{};
    const HWND start = FindWindowExW(bar, nullptr, L"Start", nullptr);
    const bool hasNativeStart = start && GetWindowRect(start, &nativeStart) &&
                                Overlaps(nativeStart, result.bounds);
    const auto axis = result.Axis();
    bool foundStart = false;
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IUIAutomationElement> item;
        CONTROLTYPEID type = 0;
        RECT bounds{};
        if (FAILED(items->GetElement(i, item.put())) || !item ||
            FAILED(item->get_CachedControlType(&type)) || FAILED(item->get_CachedBoundingRectangle(&bounds))) continue;
        OffsetRect(&bounds, -offset.x, -offset.y);
        if (!Overlaps(bounds, result.dock) || AxisStart(bounds, axis) >= AxisStart(result.notification, axis) ||
            AxisEnd(bounds, axis) > AxisEnd(result.dock, axis)) continue;
        if (type != UIA_ButtonControlTypeId && type != UIA_SplitButtonControlTypeId) continue;
        BSTR idText = nullptr;
        if (SUCCEEDED(item->get_CachedAutomationId(&idText)) && idText) {
            if (std::wstring_view(idText, SysStringLen(idText)) == L"StartButton") {
                foundStart = !hasNativeStart || Near(AxisStart(bounds, axis), AxisStart(nativeStart, axis), 12);
                result.startButton = bounds;
            }
            SysFreeString(idText);
        }
        result.buttons.push_back(bounds);
    }
    if (!foundStart) return {};
    MergeButtonSpans(result);
    return result;
}

} // namespace

TaskbarLayout ReadTaskbarLayout(HWND bar, IconOffset offset) {
    // UI Automation is called on a worker. Explorer's UI thread supplies the
    // accessibility elements and must remain free to paint and process input.
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) return {};
    TaskbarLayout layout;
    try {
        layout = Measure(bar, offset);
    } catch (...) {
        layout = {};
    }
    CoUninitialize();
    return layout;
}

} // namespace fullwintb::appearance
