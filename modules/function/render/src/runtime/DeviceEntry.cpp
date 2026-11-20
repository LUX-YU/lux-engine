#include <algorithm>
#include <exception>
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
        if (!variants_.empty()) std::terminate();
    }

    EffectDeviceVariant* DeviceEntry::findVariant(
        EffectId effect, const EffectOutputDesc& target, const runtime_detail::EffectImportContracts& imports
    ) noexcept
    {
        const auto found = std::ranges::find_if(variants_, [&](const auto& variant) noexcept
        {
            return !variant.failure.type && variant.effect == effect && variant.target == target &&
                variant.imports == imports;
        });
        return found != variants_.end() ? &*found : nullptr;
    }

    EffectDeviceVariant& DeviceEntry::beginVariant(
        EffectId effect, const EffectOutputDesc& target, const runtime_detail::EffectImportContracts& imports
    ) noexcept
    {
        const auto id = variants_.emplace(EffectDeviceVariant{.effect = effect, .target = target, .imports = imports});
        auto& variant = *variants_.find(id);
        variant.id = id;
        return variant;
    }

    const runtime_detail::JobGraphPlan& DeviceEntry::variantPlan(DeviceVariantId id) const noexcept
    {
        const auto* variant = variants_.find(id);
        if (!variant || !variant->plan) std::terminate();
        return *variant->plan;
    }

    void DeviceEntry::releaseVariant(DeviceVariantId id) noexcept
    {
        auto* variant = variants_.find(id);
        if (!variant || !variant->readers) std::terminate();
        if (--variant->readers == 0)
        {
            const bool finished = variant->plan || variant->failure.type;
            if (!finished || !variant->waiters.empty()) std::terminate();
            variants_.erase(id);
        }
    }

    UniformBlockVersions* DeviceEntry::uniformPool(EffectId effect, graph::NodeId node, std::string_view name) noexcept
    {
        const auto found = std::ranges::find_if(uniform_pools_, [&](const auto& pool) noexcept
        {
            return pool.effect == effect && pool.node == node && pool.name == name;
        });
        return found != uniform_pools_.end() ? found->versions.get() : nullptr;
    }

    void DeviceEntry::adoptUniformPools(std::vector<UniformPool> pools) noexcept
    {
        for (auto& pool : pools) uniform_pools_.push_back(std::move(pool));
    }

    void DeviceEntry::removeUniformPools(EffectId effect) noexcept
    {
        std::erase_if(uniform_pools_, [effect](const auto& pool) noexcept { return pool.effect == effect; });
    }

    void DeviceEntry::clearUniformPools() noexcept
    {
        if (!variants_.empty()) std::terminate();
        uniform_pools_.clear();
    }

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

    bool DeviceEntry::hasBorrows() const noexcept
    {
        const bool has_uploads = upload_pending || (uploader_ && uploader_->hasPendingTransfers());
        return pending_starts || has_uploads || resources_.hasBorrows() || scopedResourceBorrows() ||
            round.operation || !nativeIdle();
    }
} // namespace lux::render::detail
