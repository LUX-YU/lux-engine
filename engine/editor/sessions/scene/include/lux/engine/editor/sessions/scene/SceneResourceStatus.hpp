#pragma once
#include <lux/engine/editor/sessions/scene/SceneSession.hpp>
#include <lux/engine/process/TaskScope.hpp>
namespace lux::editor::sessions
{
    struct ResourceRequestKey final
    {
        SceneEntityRef target;
        lux::asset::AssetId mesh, material;
        std::uint64_t sequence{};
        friend bool operator==(const ResourceRequestKey &, const ResourceRequestKey &) noexcept = default;
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
        std::optional<lux::process::asset_loading::AssetLoadFailure> asset_failure;
        std::optional<lux::process::ETaskStartError> process_failure;
        std::optional<lux::render::ERenderUploadSubmitError> upload_failure;
        lux::render::RenderError render_failure;
        std::uint32_t backend_status{};
    };
    struct SceneResourceSnapshot final
    {
        SessionId session;
        std::uint64_t revision{};
        std::vector<SceneResourceRow> rows;
    };
} // namespace lux::editor::sessions
