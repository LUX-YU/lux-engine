#pragma once

#include <lux/engine/editor/DocumentRequests.hpp>
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/scene/SceneEdit.hpp>
#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>

namespace lux::render
{
    enum class ERenderUploadSubmitError : std::uint8_t;
}

namespace lux::editor::scene
{
    enum class ESceneError : std::uint8_t
    {
        INVALID_ARGUMENT,
        BUSY,
        NOT_READY,
        CLOSED,
        STALE_DOCUMENT,
        STALE_CONTENT,
        RESOURCE_FAILURE
    };

    struct SceneFailure final
    {
        ESceneError code{};
        editing::HistoryId history;
    };

    template <class T> using SceneResult = lux::cxx::expected<T, SceneFailure>;

    struct ResourceRequestKey final
    {
        SceneEntityRef target;
        lux::asset::AssetId mesh, material;
        std::uint64_t sequence{};
        friend bool operator==(const ResourceRequestKey &, const ResourceRequestKey &) = default;
    };

    enum class ESceneResourceState : std::uint8_t
    {
        UNREFERENCED,
        READING,
        UPLOADING,
        READY,
        FAILED,
        CANCELLED,
        SUPERSEDED,
        RELEASING,
        RELEASED
    };

    struct SceneResourceRow final
    {
        ResourceRequestKey key;
        ESceneResourceState state{ESceneResourceState::READING};
        std::variant<std::monostate, lux::process::asset_loading::AssetLoadFailure, lux::process::ETaskStartError,
                     lux::render::ERenderUploadSubmitError>
            failure;
        lux::render::RenderError render_failure;
        std::uint32_t backend_status{};
        lux::asset::AssetId failed_dependency;
        bool refresh_pending{};
    };

    struct SceneResourceSnapshot final
    {
        editing::HistoryId history;
        std::uint64_t revision{};
        std::vector<SceneResourceRow> rows;
    };

    struct SceneResourceCloseRow final
    {
        SceneResourceRow resource;
        bool mesh_read_pending{}, material_read_pending{};
        bool mesh_upload_pending{}, material_upload_pending{}, forward_upload_pending{}, gbuffer_upload_pending{};
        bool retirement_pending{};
        std::size_t live_handles{}, run_pins{};
        std::size_t texture_reads_pending{}, texture_uploads_pending{};
    };

    struct SceneCloseSnapshot final
    {
        editing::HistoryId history;
        ECloseState state{ECloseState::CLOSED};
        std::size_t views{};
        bool scene_present{}, retirement_submission_pending{}, task_scope_complete{true};
        std::vector<SceneResourceCloseRow> resources;
    };
} // namespace lux::editor::scene
