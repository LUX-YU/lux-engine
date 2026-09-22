#pragma once

#include <atomic>
#include <lux/engine/function/render/client/RenderLease.hpp>

namespace lux::render::detail
{
struct ResourceReleaseRecord final
{
    // Declare code first: the concrete payload's destructor runs before its
    // implementation module can be unloaded.
    std::shared_ptr<const void> code;
    std::shared_ptr<void> payload;
    RenderResourceReleaseStep release{};
    std::atomic<std::size_t> uses{1};
};

struct SceneReleaseRecord
{
    RenderSceneId scene;
    bool requested{};
    bool submitted{};
    std::atomic<std::size_t> uses{1};
    ESceneResourceState state{ESceneResourceState::READY};
    RenderError failure;
    std::uint64_t failed_request{};
    FeatureTypeId failed_feature{};
    CreateScenePayload creation;
    std::vector<SceneFeatureAttachment> attachments;
    std::vector<std::pair<FeatureTypeId, FeatureHandle>> features;
    std::size_t next{};
    RenderRequest<SceneCreatedReply> create;
    RenderRequest<FeatureAddedReply> attach;
    RenderRequest<GenericOkReply> release;

    SceneResourceStatus status() const noexcept
    {
        return {state, scene, failure, failed_request, failed_feature};
    }

    void fail(RenderError error, std::uint64_t request, FeatureTypeId feature = {}) noexcept
    {
        if (failure.ok())
        {
            failure = error.ok() ? renderError<err::comm::RequestInvalid>() : error;
            failed_request = request;
            failed_feature = feature;
        }
        requested = true;
    }
};

struct ViewReleaseRecord
{
    RenderSceneId scene;
    ViewHandle view;
    RenderViewReleaseObserver observer;
    bool requested{};
    bool owned{true};
    bool submitted{};
};

struct TargetReleaseRecord
{
    RenderTargetId target;
    RenderTargetReleaseObserver observer;
    RenderRequest<TargetReleasedReply> request;
    bool requested{};
    bool owned{true};
    bool submitted{};
};
} // namespace lux::render::detail
