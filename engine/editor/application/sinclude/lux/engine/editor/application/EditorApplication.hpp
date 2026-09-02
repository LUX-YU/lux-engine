#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/process/asset_loading/VfsAssetReadEndpoint.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lux::editor
{
    namespace application::detail
    {
        class UiVulkanPresentation;
    }

    enum class EEditorApplicationError : std::uint8_t
    {
        INVALID_STATE,
        EXECUTION_CREATE_FAILURE,
        VFS_MOUNT_FAILURE,
        ASSET_READ_CREATE_FAILURE,
        WINDOW_RUNTIME_CREATE_FAILURE,
        WINDOW_CREATE_FAILURE,
        PRESENTATION_CREATE_FAILURE,
        PRESENTATION_FRAME_FAILURE,
        PRESENTATION_JOIN_FAILURE,
        ALLOCATION_FAILURE,
        TASK_CLOSE_FAILURE,
        ASSET_READ_JOIN_FAILURE,
        EXECUTION_JOIN_FAILURE,
    };

    struct EditorPresentationConfig final
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::string title;
        std::size_t frame_capacity{};
        std::size_t control_capacity{};
        std::size_t upload_capacity{};
        std::size_t upload_byte_capacity{};
        render::ProgramMemoryHints program_memory;
        bool visible{true};
        bool enable_validation{};
    };

    struct EditorApplicationCreateInfo final
    {
        process::ExecutionRuntimeConfig execution;
        process::asset_loading::VfsAssetReadEndpointConfig asset_read;
        scene::SceneMetaManager scene_meta;
        std::vector<asset::MountDesc> mounts;
        std::optional<EditorPresentationConfig> presentation;
    };

    class EditorApplication final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<EditorApplication>, EEditorApplicationError>;
        using ContextResult = lux::cxx::expected<std::reference_wrapper<EditorContext>, EEditorApplicationError>;

        [[nodiscard]] static CreateResult create(EditorApplicationCreateInfo info) noexcept;

        ~EditorApplication() noexcept;
        EditorApplication(const EditorApplication&) = delete;
        EditorApplication& operator=(const EditorApplication&) = delete;
        EditorApplication(EditorApplication&&) = delete;
        EditorApplication& operator=(EditorApplication&&) = delete;

        template<class Tool, class... Args>
        [[nodiscard]] lux::cxx::expected<std::reference_wrapper<Tool>, ToolsetFailure>
        installTool(Args&&... args) noexcept
        {
            const auto type = lux::cxx::typeToken<Tool>();
            if (state_ == EState::RUNNING)
            {
                return lux::cxx::unexpected(ToolsetFailure{EToolsetError::FROZEN, type, {}});
            }
            if (state_ != EState::COMPOSING || !toolset_)
            {
                return lux::cxx::unexpected(ToolsetFailure{EToolsetError::STOPPING, type, {}});
            }
            return toolset_->install<Tool>(std::forward<Args>(args)...);
        }

        [[nodiscard]] lux::cxx::expected<void, EEditorApplicationError> start() noexcept;
        [[nodiscard]] ContextResult context() noexcept;
        [[nodiscard]] lux::cxx::expected<std::size_t, EEditorApplicationError>
        drainMain(std::size_t budget = static_cast<std::size_t>(-1)) noexcept;
        [[nodiscard]] lux::cxx::expected<std::size_t, EEditorApplicationError>
        run(std::size_t max_frames = 0U) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EEditorApplicationError> shutdown() noexcept;

    private:
        enum class EState : std::uint8_t
        {
            COMPOSING,
            RUNNING,
            STOPPING,
            JOINED,
        };

        EditorApplication(
            process::ExecutionRuntime runtime,
            scene::SceneMetaManager scene_meta,
            std::optional<EditorPresentationConfig> presentation
        );
        [[nodiscard]] bool closeRootTasks() noexcept;
        void feedWindowInput() noexcept;
        void clearWindowCallbacks() noexcept;

        process::ExecutionRuntime execution_;
        asset::AssetVfs vfs_;
        std::shared_ptr<process::asset_loading::VfsAssetReadEndpoint> asset_read_endpoint_;
        scene::SceneMetaManager scene_meta_;
        std::optional<ui::UISession> ui_;
        std::optional<EditorSelection> selection_;
        std::optional<Toolset> toolset_;
        std::optional<process::TaskScope> tasks_;
        std::optional<EditorContext> context_;
        std::optional<EditorPresentationConfig> presentation_config_;
        struct PresentationOwners;
        std::unique_ptr<PresentationOwners> presentation_owners_;
        EState state_{EState::COMPOSING};
    };
} // namespace lux::editor
