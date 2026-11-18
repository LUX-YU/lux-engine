#include <algorithm>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/device/NativeExecution.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>

namespace lux::render::detail
{
    DeviceEntry::DeviceEntry(
        ResourceRegistry& registry,
        std::unique_ptr<NativeExecution> native,
        std::unique_ptr<DeviceSession> session,
        std::uint32_t frame_count,
        std::size_t preparation_capacity
    ) noexcept
        : native_(std::move(native)), session_(std::move(session)),
          frame_progress_(frame_count, session_.get(), session_->queueCount()),
          frame_workspace_(frame_count, session_->queueCount(), preparation_capacity),
          resources_(registry, *session_)
    {
    }

    RenderResult<std::unique_ptr<DeviceEntry>> DeviceEntry::create(
        ResourceRegistry& registry,
        const DeviceOptions& options,
        const nri::CallbackInterface& callbacks,
        const RenderServerConfig& config,
        execution::CpuScheduler cpu
    ) noexcept
    {
        auto native = NativeExecution::create();
        if (!native)
        {
            return cxx::unexpected(native.error());
        }
        auto session = DeviceSession::create(options, callbacks, config.validation, (*native)->scheduler());
        if (!session)
        {
            return cxx::unexpected(session.error());
        }
        const auto preparation_capacity = config.uploads ? config.uploads->window_regions : 0;
        auto entry = std::unique_ptr<DeviceEntry>(
            new DeviceEntry(registry, std::move(*native), std::move(*session), config.frames_in_flight,
                preparation_capacity)
        );
        if (config.uploads)
        {
            auto uploader = ResourceUploader::create(entry->session(), *config.uploads, cpu);
            if (!uploader)
            {
                return cxx::unexpected(uploader.error());
            }
            entry->uploader_ = std::move(*uploader);
        }
        return entry;
    }

    DeviceEntry::~DeviceEntry() noexcept = default;

    ResourceCreateContext DeviceEntry::resourceCreateContext() noexcept
    {
        return {session(), uploader(), frame_progress_.frameCount(), resources_.registry(), &frame_progress_};
    }

    ResourceSet* DeviceEntry::scopedResources(SceneId scene, ViewId view, bool create) noexcept
    {
        if (scene.isNull()) return &resources_;
        const ScopeKey scene_key{scene.index, scene.gen};
        auto parent = create ? scene_resources_.try_emplace(scene_key, resources_).first :
            scene_resources_.find(scene_key);
        if (parent == scene_resources_.end()) return nullptr;
        auto& scope = parent->second;
        if (view.isNull()) return &scope.resources;
        const ScopeKey view_key{view.index, view.gen};
        auto child = create ? scope.views.try_emplace(view_key, scope.resources, EResourceDomain::VIEW).first :
            scope.views.find(view_key);
        return child == scope.views.end() ? nullptr : &child->second;
    }

    bool DeviceEntry::scopedResourceCommands(SceneId scene, ViewId view) const noexcept
    {
        const auto has_commands = [](const SceneResources& scope) noexcept
        {
            return scope.resources.hasCommands() ||
                std::ranges::any_of(scope.views, [](const auto& child) noexcept { return child.second.hasCommands(); });
        };
        if (scene.isNull())
        {
            return std::ranges::any_of(scene_resources_, [&](const auto& entry) noexcept
            {
                return has_commands(entry.second);
            });
        }

        const auto found = scene_resources_.find({scene.index, scene.gen});
        if (found == scene_resources_.end()) return false;
        if (view.isNull()) return has_commands(found->second);
        const auto child = found->second.views.find({view.index, view.gen});
        return child != found->second.views.end() && child->second.hasCommands();
    }

    void DeviceEntry::eraseResourceScope(SceneId scene, ViewId view) noexcept
    {
        const ScopeKey key{scene.index, scene.gen};
        if (view.isNull()) scene_resources_.erase(key);
        else if (auto parent = scene_resources_.find(key); parent != scene_resources_.end())
            parent->second.views.erase({view.index, view.gen});
    }

    bool DeviceEntry::nativeIdle() const noexcept
    {
        return native_->idle();
    }
} // namespace lux::render::detail
