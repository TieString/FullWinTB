#pragma once

#include "taskbar_layout.h"

#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <vector>

namespace fullwintb::appearance {

class TaskbarIconInk {
public:
    void Track(std::uint64_t id, IconGroup group, const winrt::Windows::UI::Xaml::Controls::Image& image);
    void Forget(std::uint64_t id) noexcept;
    void SetInk(Ink apps, Ink notification);

private:
    using Image = winrt::Windows::UI::Xaml::Controls::Image;
    struct Entry {
        std::uint64_t id;
        IconGroup group;
        winrt::weak_ref<Image> image;
        winrt::event_token opened{};
        winrt::event_token loaded{};
        std::int64_t sourceWatch = 0;
        unsigned revision = 0;
        Ink natural = Ink::system;
        bool pending = true;
        winrt::Windows::UI::Composition::SpriteVisual tint{nullptr};
    };

    Entry* Find(std::uint64_t id);
    Ink Wanted(IconGroup group) const { return group == IconGroup::apps ? apps_ : notification_; }
    void SourceChanged(std::uint64_t id) noexcept;
    void ImageReady(std::uint64_t id) noexcept;
    void ScheduleScan() noexcept;
    winrt::fire_and_forget Scan(std::uint64_t id, unsigned revision, Image image);
    void Paint(Entry& entry) noexcept;
    static void RemoveTint(Entry& entry) noexcept;

    std::vector<Entry> entries_;
    Ink apps_ = Ink::system;
    Ink notification_ = Ink::system;
    bool scanning_ = false;
    bool scanQueued_ = false;
};

} // namespace fullwintb::appearance
