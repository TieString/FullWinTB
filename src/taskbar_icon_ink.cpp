#include "taskbar_icon_ink.h"
#include "icon_ink.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

namespace fullwintb::appearance {
namespace {

using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview;
using winrt::Windows::UI::Xaml::Media::Imaging::RenderTargetBitmap;

constexpr int probeSize = 32;

bool CanSample(const winrt::Windows::UI::Xaml::Controls::Image& image) {
    return image && image.IsLoaded() && image.Source() && image.ActualWidth() >= 1 && image.ActualHeight() >= 1;
}

} // namespace

TaskbarIconInk::Entry* TaskbarIconInk::Find(std::uint64_t id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                   [id](const Entry& entry) { return entry.id == id; });
    return found == entries_.end() ? nullptr : &*found;
}

void TaskbarIconInk::Track(std::uint64_t id, IconGroup group, const Image& image) {
    if (Find(id) || entries_.size() >= 256) return;
    entries_.push_back(Entry{id, group, winrt::make_weak(image)});
    auto& entry = entries_.back();
    try {
        entry.sourceWatch = image.RegisterPropertyChangedCallback(Image::SourceProperty(),
            [this, id](const auto&, const auto&) { SourceChanged(id); });
        entry.opened = image.ImageOpened([this, id](const auto&, const auto&) { ImageReady(id); });
        entry.loaded = image.Loaded([this, id](const auto&, const auto&) { ImageReady(id); });
    } catch (...) {
        Forget(id);
        throw;
    }
    ScheduleScan();
}

void TaskbarIconInk::Forget(std::uint64_t id) noexcept try {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                   [id](const Entry& entry) { return entry.id == id; });
    if (found == entries_.end()) return;
    Entry entry = std::move(*found);
    entries_.erase(found);
    RemoveTint(entry);
    if (const auto image = entry.image.get()) {
        if (entry.opened) image.ImageOpened(entry.opened);
        if (entry.loaded) image.Loaded(entry.loaded);
        if (entry.sourceWatch)
            image.UnregisterPropertyChangedCallback(Image::SourceProperty(), entry.sourceWatch);
    }
} catch (...) {
    // The visual can already have been destroyed by Explorer.
}

void TaskbarIconInk::SetInk(Ink apps, Ink notification) {
    if (apps_ == apps && notification_ == notification) return;
    apps_ = apps;
    notification_ = notification;
    for (auto& entry : entries_) Paint(entry);
    ScheduleScan();
}

void TaskbarIconInk::SourceChanged(std::uint64_t id) noexcept {
    if (auto* entry = Find(id)) {
        RemoveTint(*entry);
        entry->natural = Ink::system;
        ++entry->revision;
        entry->pending = true;
        ScheduleScan();
    }
}

void TaskbarIconInk::ImageReady(std::uint64_t id) noexcept {
    if (auto* entry = Find(id); entry && entry->natural == Ink::system) {
        ++entry->revision;
        entry->pending = true;
        ScheduleScan();
    }
}

void TaskbarIconInk::ScheduleScan() noexcept try {
    if (scanning_ || scanQueued_) return;
    for (const auto& entry : entries_) {
        if (!entry.pending || Wanted(entry.group) == Ink::system) continue;
        const auto image = entry.image.get();
        if (!CanSample(image)) continue;
        scanQueued_ = true;
        image.Dispatcher().RunAsync(CoreDispatcherPriority::Low, [this, id = entry.id] {
            scanQueued_ = false;
            if (scanning_) return;
            try {
                if (auto* item = Find(id); item && item->pending && Wanted(item->group) != Ink::system) {
                    if (const auto current = item->image.get(); CanSample(current)) {
                        item->pending = false;
                        scanning_ = true;
                        Scan(item->id, item->revision, current);
                        return;
                    }
                }
            } catch (...) {
                scanning_ = false;
            }
            ScheduleScan();
        });
        return;
    }
} catch (...) {
    scanQueued_ = false;
}

winrt::fire_and_forget TaskbarIconInk::Scan(std::uint64_t id, unsigned revision, Image image) {
    Ink natural = Ink::system;
    try {
        RenderTargetBitmap bitmap;
        const auto root = image.XamlRoot();
        const double scale = root ? std::max(1.0, root.RasterizationScale()) : 1.0;
        // XAML expresses the render request in effective pixels.
        const int size = std::max(1, static_cast<int>(probeSize / scale));
        co_await bitmap.RenderAsync(image, size, size);
        const auto pixels = co_await bitmap.GetPixelsAsync();
        if (pixels && pixels.Length() <= probeSize * probeSize * 4)
            natural = ClassifyIconInk({pixels.data(), pixels.Length()});
    } catch (...) {
        // A source can disappear while Explorer rebuilds the taskbar.
    }
    scanning_ = false;
    try {
        if (auto* entry = Find(id); entry && entry->revision == revision && entry->image.get() == image) {
            entry->natural = natural;
            Paint(*entry);
        }
    } catch (...) {
        // The icon may have been recycled while its pixels were being read.
    }
    ScheduleScan();
}

void TaskbarIconInk::RemoveTint(Entry& entry) noexcept try {
    if (!entry.tint) return;
    // An unlinked hand-in visual can still be present in the compositor.
    entry.tint.Opacity(0);
    const auto image = entry.image.get();
    if (image && ElementCompositionPreview::GetElementChildVisual(image) == entry.tint)
        ElementCompositionPreview::SetElementChildVisual(image, nullptr);
    entry.tint = nullptr;
} catch (...) {
    entry.tint = nullptr;
}

void TaskbarIconInk::Paint(Entry& entry) noexcept try {
    const auto wanted = Wanted(entry.group);
    if (wanted == Ink::system || entry.natural == Ink::system || entry.natural == wanted) {
        RemoveTint(entry);
        return;
    }
    const auto image = entry.image.get();
    if (entry.tint || !image || ElementCompositionPreview::GetElementChildVisual(image)) return;
    const auto mask = image.GetAlphaMask();
    if (!mask) return;
    const auto compositor = ElementCompositionPreview::GetElementVisual(image).Compositor();
    const std::uint8_t shade = wanted == Ink::white ? 255 : 0;
    const auto brush = compositor.CreateMaskBrush();
    brush.Source(compositor.CreateColorBrush({255, shade, shade, shade}));
    brush.Mask(mask);
    const auto visual = compositor.CreateSpriteVisual();
    visual.RelativeSizeAdjustment({1.0f, 1.0f});
    visual.Brush(brush);
    ElementCompositionPreview::SetElementChildVisual(image, visual);
    entry.tint = visual;
} catch (...) {
    RemoveTint(entry);
}

} // namespace fullwintb::appearance
