#include <lux/engine/render/device/RasterProgramUpdate.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <algorithm>
#include <exception>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
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
        round.target_uses = session_->makeFrontiers();
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

    DeviceEntry::~DeviceEntry() noexcept
    {
        if (!job_resources_.empty() || !pending_program_bindings_.empty()) std::terminate();
    }

    void DeviceEntry::activateProgramBinding(ExecutionBindingId id) noexcept
    {
        pending_program_bindings_.push_back(id);
    }

    void DeviceEntry::retireProgramBinding(ExecutionBindingId id) noexcept
    {
        const auto found = std::ranges::find(pending_program_bindings_, id);
        if (found == pending_program_bindings_.end()) std::terminate();
        const auto removed = static_cast<std::size_t>(found - pending_program_bindings_.begin());
        pending_program_bindings_.erase(found);
        auto& cursor = round.preparation_cursor;
        if (removed < cursor) --cursor;
        if (cursor >= pending_program_bindings_.size()) cursor = 0;
    }

    std::unique_ptr<JobGpuResources> DeviceEntry::prepareJobResources(ExecutionBindingId identity) noexcept
    {
        if (!job_resources_.contains(identity.job))
            job_resources_.prepareCapacity(job_resources_.size() + 1, std::size_t(identity.job.index) + 1);
        return std::make_unique<JobGpuResources>(session(), frame_progress_.frameCount(), identity.generation);
    }

    std::unique_ptr<JobGpuResources>
    DeviceEntry::installJobResources(RenderJobId job, std::unique_ptr<JobGpuResources> candidate) noexcept
    {
        if (auto* current = job_resources_.tryGet(job))
        {
            if ((*current)->generation >= candidate->generation) std::terminate();
            return std::exchange(*current, std::move(candidate));
        }
        job_resources_.emplace(job, std::move(candidate));
        return {};
    }

    JobGpuResources& DeviceEntry::jobResources(ExecutionBindingId identity) & noexcept
    {
        const auto* resources = job_resources_.tryGet(identity.job);
        if (!resources || (*resources)->generation != identity.generation) std::terminate();
        return **resources;
    }

    std::unique_ptr<JobGpuResources> DeviceEntry::takeJobResources(ExecutionBindingId identity) noexcept
    {
        auto* current = job_resources_.tryGet(identity.job);
        if (!current || (*current)->generation != identity.generation) std::terminate();
        auto retired = std::move(*current);
        job_resources_.erase(identity.job);
        return retired;
    }

    void DeviceEntry::installEffectResources(EffectId effect, const PreparationPlan& plan) noexcept
    {
        if (plan.constant_buffers.empty()) return;
        EffectBuffers buffers;
        buffers.reserve(plan.constant_buffers.size());
        const auto frame_count = frame_progress_.frameCount();
        for (const auto& input : plan.constant_buffers)
        {
            buffers.push_back(std::make_unique<ConstantBufferStorage>(session(), input.bytes, frame_count));
        }
        effect_buffers_.prepareCapacity(effect_buffers_.size() + 1, std::size_t(effect.index) + 1);
        effect_buffers_.emplace(effect, std::move(buffers));
    }

    std::span<const std::unique_ptr<ConstantBufferStorage>>
    DeviceEntry::effectResources(EffectId effect) const & noexcept
    {
        const auto* buffers = effect_buffers_.tryGet(effect);
        return buffers ? std::span<const std::unique_ptr<ConstantBufferStorage>>{*buffers} :
            std::span<const std::unique_ptr<ConstantBufferStorage>>{};
    }

    void DeviceEntry::removeEffectResources(EffectId effect) noexcept
    {
        if (effect_buffers_.contains(effect)) effect_buffers_.erase(effect);
    }

    ResourceCreateContext DeviceEntry::resourceCreateContext() noexcept
    {
        return ResourceCreateContext(*this);
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

    bool DeviceEntry::scopedResourceBorrows(SceneId scene, ViewId view) const noexcept
    {
        const auto has_resource_borrows = [](const SceneResources& scope) noexcept
        {
            return scope.resources.hasBorrows() ||
                std::ranges::any_of(scope.views, [](const auto& child) noexcept { return child.second.hasBorrows(); });
        };
        if (scene.isNull())
        {
            return std::ranges::any_of(scene_resources_, [&](const auto& entry) noexcept
            {
                return has_resource_borrows(entry.second);
            });
        }

        const auto found = scene_resources_.find({scene.index, scene.gen});
        if (found == scene_resources_.end()) return false;
        if (view.isNull()) return has_resource_borrows(found->second);
        const auto child = found->second.views.find({view.index, view.gen});
        return child != found->second.views.end() && child->second.hasBorrows();
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

    bool DeviceEntry::preparingProgramsFor(ExecutionBindingId binding) const noexcept
    {
        return program_update && program_update->binding == binding;
    }

    bool DeviceEntry::hasBorrows() const noexcept
    {
        const bool has_uploads = upload_pending || (uploader_ && uploader_->hasPendingTransfers());
        return program_update || !job_resources_.empty() || has_uploads || resources_.hasBorrows() ||
            scopedResourceBorrows() || round.operation || !nativeIdle();
    }
} // namespace lux::render::detail

namespace lux::render
{
    ResourceCreateContext::ResourceCreateContext(detail::DeviceEntry& entry) noexcept
        : device(entry.session()), uploads(entry.uploader()), frame_count(entry.frameProgress().frameCount()),
          resources(entry.resources().registry()), retirement_progress_(entry.frameProgress())
    {
    }
}
