#include <lux/engine/scene/detail/RenderResourcesImpl.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace lux::scene
{
    SceneResourceStatus RenderSceneReceipt::status() const noexcept
    {
        return record_ ? *record_ : SceneResourceStatus{ESceneResourceState::RETIRED};
    }

    void detail::SceneState::fail(
        render::RenderError error,
        std::uint64_t request,
        render::FeatureTypeId feature
    ) noexcept
    {
        if (status->failure.ok())
        {
            status->failure = error.ok() ? render::renderError<render::err::comm::RequestInvalid>() : error;
            status->request = request;
            status->feature = feature;
        }
    }

    render::RenderResult<RenderResourceId> RenderResources::requestScene(
        const render::RenderControlSession::CreateSceneConfig& config,
        std::vector<SceneFeatureAttachment> attachments
    ) noexcept
    {
        auto& self = *impl_;
        self.requireOwner();
        if (self.closing || self.runtime.status().state != render::ERenderRuntimeState::ACTIVE)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::STOPPING});
        if (!std::isfinite(config.coordinate_page_size) || config.coordinate_page_size <= 0)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::INVALID_ARGUMENT});
        if (self.records.size() == self.limits.requests)
            return lux::cxx::unexpected(render::RendererFailure{render::ERendererError::CAPACITY});
        // Admission allocates the complete record before any control request.
        {
            auto record = std::make_unique<Impl::Record>();
            auto& scene = record->payload.emplace<detail::SceneState>();
            scene.status = std::make_shared<SceneResourceStatus>();
            scene.creation = {};
            if (config.name)
                std::strncpy(scene.creation.name, config.name, sizeof(scene.creation.name) - 1);
            scene.creation.flags = config.flags;
            scene.creation.lit_color_format = config.lit_color_format;
            scene.creation.coordinate_page_size = config.coordinate_page_size;
            std::copy_n(config.scene_origin_page, 3, scene.creation.scene_origin_page);
            scene.attachments = std::move(attachments);
            scene.features.reserve(scene.attachments.size());
            const auto key = self.records.emplace(std::move(record));
            auto& admitted = *self.records[key];
            admitted.id = {self.domain, key};
            self.enqueue(admitted);
            return admitted.id;
        }
    }

    RenderSceneReceipt RenderResources::sceneReceipt(RenderResourceId id) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* scene = record ? std::get_if<detail::SceneState>(&record->payload) : nullptr;
        RenderSceneReceipt result;
        if (scene)
            result.record_ = scene->status;
        return result;
    }

    render::FeatureHandle RenderResources::sceneFeature(RenderResourceId id, render::FeatureTypeId type) const noexcept
    {
        const auto* record = impl_->find(id);
        const auto* scene = record ? std::get_if<detail::SceneState>(&record->payload) : nullptr;
        if (scene && scene->status->state == ESceneResourceState::READY && scene->status->failure.ok())
            for (const auto& [identity, handle] : scene->features)
                if (identity == type)
                    return handle;
        return {};
    }

    void RenderResources::Impl::advanceScene(Record& record) noexcept
    {
        auto& scene = std::get<detail::SceneState>(record.payload);
        auto& status = *scene.status;
        if (status.state == ESceneResourceState::RETIRED)
            return;
        const auto backend = runtime.status();
        if (backend.state != render::ERenderRuntimeState::ACTIVE)
        {
            scene.fail(
                backend.error.ok() ? render::renderError<render::err::comm::ChannelStopping>() : backend.error,
                0
            );
            // Stop intent is insufficient. Runtime RETIRED proves worker join
            // and retirement of accepted CPU packets as well as backend objects.
            if (backend.state == render::ERenderRuntimeState::RETIRED)
            {
                scene.create = {};
                scene.attach = {};
                scene.release = {};
                status.state = ESceneResourceState::RETIRED;
            }
            return;
        }
        if (scene.create.valid() && scene.create.isReady())
        {
            auto result = scene.create.tryResult();
            if (!result || !result->get().error.ok() || !result->get().scene_id.isValid())
                scene.fail(result ? result->get().error : result.error(), scene.create.requestId());
            else
            {
                status.scene = result->get().scene_id;
                status.state = ESceneResourceState::ATTACHING;
            }
            scene.create = {};
        }
        if (scene.attach.valid() && scene.attach.isReady())
        {
            auto result = scene.attach.tryResult();
            const auto type = scene.attachments[scene.next].type;
            if (!result || !result->get().error.ok() || !result->get().feature.isValid())
                scene.fail(result ? result->get().error : result.error(), scene.attach.requestId(), type);
            else
            {
                scene.features.emplace_back(type, result->get().feature);
                ++scene.next;
            }
            scene.attach = {};
        }
        if (scene.release.valid() && scene.release.isReady())
        {
            auto result = scene.release.tryResult();
            if (!result || result->get().code != 0 || !result->get().error.ok())
            {
                scene.fail(result ? result->get().error : result.error(), scene.release.requestId());
                // Destruction was rejected. Never retry an irreversible command;
                // retire the backend before discarding the remaining obligation.
                runtime.control()->get().requestStop();
            }
            else
                status.state = ESceneResourceState::RETIRED;
            scene.release = {};
        }
        if (!record.references)
        {
            std::erase_if(record.submissions, [](const auto& use) { return use.complete(); });
            const bool pending = !record.submissions.empty() || scene.create.valid() || scene.attach.valid();
            const bool release_started =
                status.state == ESceneResourceState::RELEASING || status.state == ESceneResourceState::RETIRED;
            if (pending || release_started)
                return;
            if (!status.scene.isValid())
            {
                status.state = ESceneResourceState::RETIRED;
                return;
            }
            if (!runtime.controlAvailable())
                return;
            scene.release = runtime.control()->get().destroyScene(status.scene);
            status.state = ESceneResourceState::RELEASING;
            return;
        }
        if (!status.failure.ok())
            return;
        if (status.state == ESceneResourceState::QUEUED && runtime.controlAvailable())
        {
            const auto& value = scene.creation;
            render::RenderControlSession::CreateSceneConfig config{
                .name = value.name,
                .flags = value.flags,
                .lit_color_format = value.lit_color_format,
                .coordinate_page_size = value.coordinate_page_size
            };
            std::copy_n(value.scene_origin_page, 3, config.scene_origin_page);
            scene.create = runtime.control()->get().createScene(config);
            status.state = ESceneResourceState::CREATING;
        }
        if (status.state == ESceneResourceState::ATTACHING && !scene.attach.valid())
        {
            if (scene.next == scene.attachments.size())
                status.state = ESceneResourceState::READY;
            else if (runtime.controlAvailable())
            {
                const auto& feature = scene.attachments[scene.next];
                scene.attach = runtime.control()->get().addFeatureRaw(
                    status.scene,
                    feature.registered_type,
                    feature.configuration
                );
            }
        }
    }
} // namespace lux::scene
