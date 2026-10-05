#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/description/Skeleton.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <atomic>
namespace lux::ui
{
    class Root;
    class Pane;
}
namespace lux::editor
{
    class ProjectStorage;
} // namespace lux::editor

#if defined(_WIN32)
#define SKELETON_EXPORT __declspec(dllexport)
#else
#define SKELETON_EXPORT __attribute__((visibility("default")))
#endif
namespace skeleton
{
    struct Facts final
    {
        std::atomic<unsigned> unloaded{};
        unsigned panes_created{}, panes_destroyed{}, rows_prepared{}, activations{};
        unsigned settings_applied{};
        bool indices_applied{true}, indices_displayed{true};
        // Qualification observations only. No production extension looks up capabilities through this record.
        lux::editor::ProjectStorage* project{};
        lux::ui::Root* root{};
    };
    struct Rename final
    {
        std::size_t bone{};
        std::string name;
        float x{};
    };
    using Inspect = lux::editor::sessions::SessionResult<lux::rdesc::Skeleton>(lux::editor::sessions::SessionId);
    using Describe =
        lux::editor::sessions::SessionResult<lux::editor::sessions::SessionInfo>(lux::editor::sessions::SessionId);
    using ReadGuard =
        lux::editor::sessions::SessionResult<void>(lux::editor::sessions::SessionId, void (*)(void*), void*);
    struct ProbeApi final
    {
        void (*set_facts)(Facts*) noexcept;
        Inspect* inspect;
        Describe* describe;
        ReadGuard* read_guard;
        lux::editor::views::ViewContent (*content)(const lux::ui::Pane&) noexcept;
    };
} // namespace skeleton
