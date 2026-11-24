#include <lux/engine/render/device/JobGpuResources.hpp>
#include <algorithm>
#include <exception>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/device/NativeExecution.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/RenderJob.hpp>

namespace lux::render::detail
{
    DeviceEntry::DeviceEntry(
        ResourceRegistry& registry,
        std::unique_ptr<NativeExecution> native,
        std::unique_ptr<DeviceSession> session,
        Host host
    ) noexcept
        : jobs_(host.jobs), workers_(host.workers), native_(std::move(native)), session_(std::move(session)),
          frame_progress_(host.config.frames_in_flight, session_.get(), session_->queueCount()),
          resources_(registry, *this),
          round(*session_, frame_progress_)
    {
    }

    RenderResult<std::unique_ptr<DeviceEntry>>
    DeviceEntry::create(ResourceRegistry& registry, const DeviceOptions& options, Host host) noexcept
    {
        auto native = NativeExecution::create();
        if (!native)
        {
            return cxx::unexpected(native.error());
        }
        auto session = DeviceSession::create(options, host.callbacks, host.config.validation, (*native)->scheduler());
        if (!session)
        {
            return cxx::unexpected(session.error());
        }
        auto entry = std::unique_ptr<DeviceEntry>(new DeviceEntry(registry, std::move(*native), std::move(*session), host));
        if (host.config.uploads)
        {
            auto uploader = ResourceUploader::create(entry->session(), *host.config.uploads, host.workers.recordingScheduler());
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
        if (!job_resources_.empty()) std::terminate();
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

    void DeviceEntry::installEffectResources(EffectId effect, std::span<BoundConstantBuffer> constants) noexcept
    {
        if (constants.empty()) return;
        EffectBuffers buffers;
        buffers.reserve(constants.size());
        const auto frame_count = frame_progress_.frameCount();
        for (auto& input : constants)
        {
            const auto bytes = static_cast<std::uint32_t>(input.value.bytes.size());
            auto& buffer = buffers.emplace_back(std::make_unique<ConstantBufferStorage>(session(), bytes, frame_count));
            input.storage = buffer.get();
        }
        effect_buffers_.prepareCapacity(effect_buffers_.size() + 1, std::size_t(effect.index) + 1);
        effect_buffers_.emplace(effect, std::move(buffers));
    }

    void DeviceEntry::removeEffectResources(EffectId effect) noexcept
    {
        if (effect_buffers_.contains(effect)) effect_buffers_.erase(effect);
    }

    ResourceCreateContext DeviceEntry::resourceCreateContext() noexcept
    {
        return ResourceCreateContext(*this);
    }

    ResourceSet* DeviceEntry::scopedResources(SceneId scene, ViewId view, ResourceCreateContext* creation) noexcept
    {
        if (scene.isNull()) return &resources_;
        const ScopeKey scene_key{scene.index, scene.gen};
        auto parent = creation ? scene_resources_.try_emplace(scene_key, resources_, *session_, frame_progress_.frameCount()).first :
            scene_resources_.find(scene_key);
        if (parent == scene_resources_.end()) return nullptr;
        auto& scope = parent->second;
        if (view.isNull()) return &scope.resources;
        const ScopeKey view_key{view.index, view.gen};
        if (creation)
        {
            creation->view_table_ = &scope.view_table;
            creation->view_index_ = view.index;
        }
        auto child = creation ? scope.views.try_emplace(view_key, scope.resources, EResourceDomain::VIEW).first :
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

    void DeviceEntry::eraseSceneResources(SceneId scene) noexcept
    {
        scene_resources_.erase({scene.index, scene.gen});
    }

    SubmissionFrontiers DeviceEntry::retireViewResources(SceneId scene, ViewId view) noexcept
    {
        const auto parent = scene_resources_.find({scene.index, scene.gen});
        if (parent == scene_resources_.end()) return {};
        auto& scope = parent->second;
        scope.views.erase({view.index, view.gen});
        auto uses = scope.view_table.takePendingUses(view.index);
        const auto page = view.index / SceneViewStorage::rows_per_page;
        const auto next = scope.views.lower_bound({page * SceneViewStorage::rows_per_page, 0});
        if (next == scope.views.end() || next->first.first / SceneViewStorage::rows_per_page != page)
            scope.view_table.releasePage(page);
        return uses;
    }

    std::optional<exec::task<RenderResult<void>>> DeviceEntry::startReadyUploads() noexcept
    {
        if (!uploader_ || upload_pending || !uploader_->canProgressTransfers()) return std::nullopt;
        upload_pending = true;
        return uploader_->submitReadyTransfers();
    }

    bool DeviceEntry::nativeIdle() const noexcept
    {
        return native_->idle();
    }

    bool DeviceEntry::jobUsesResource(ResourceInstanceId id) const noexcept
    {
        return std::ranges::any_of(jobs_, [id](const auto& job) noexcept { return job.usesResource(id); });
    }

    bool DeviceEntry::hasBorrows() const noexcept
    {
        const bool has_uploads = upload_pending || (uploader_ && uploader_->hasPendingTransfers());
        return !job_resources_.empty() || has_uploads || resources_.hasBorrows() ||
            scopedResourceBorrows() || round.stage != runtime_detail::DeviceRound::EStage::IDLE || !nativeIdle();
    }
} // namespace lux::render::detail

namespace lux::render
{
    ResourceCreateContext::ResourceCreateContext(detail::DeviceEntry& entry) noexcept
        : device(entry.session()), uploads(entry.uploader()), frame_count(entry.frameProgress().frameCount()),
          resources(entry.resources().registry()), device_owner_(entry), retirement_progress_(entry.frameProgress())
    {
    }
}
