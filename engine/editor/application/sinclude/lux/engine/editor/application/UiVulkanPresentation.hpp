#pragma once

#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/ui/detail/UiVulkanBackend.hpp>
#include <lux/engine/scene/RenderRuntime.hpp>
#include <lux/engine/editor/scene/SceneViewRenderPort.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::ui
{
    class UISession;
}

namespace lux::window
{
    class LuxWindow;
}

namespace lux::editor::application::detail
{
    enum class EUiVulkanPresentationError : std::uint8_t
    {
        INVALID_CONFIG,
        ALLOCATION_FAILURE,
        THREAD_CREATION_FAILURE,
        RENDER_START_FAILURE,
        FRAME_CAPTURE_FAILURE,
        FRAME_SUBMIT_FAILURE,
        STOPPING,
    };

    struct UiVulkanPresentationFailure final
    {
        EUiVulkanPresentationError code{EUiVulkanPresentationError::INVALID_CONFIG};
        render::RenderError render_error{};
    };

    struct UiVulkanPresentationConfig final
    {
        std::size_t frame_capacity{};
        std::size_t control_capacity{};
        std::size_t upload_capacity{};
        std::size_t upload_byte_capacity{};
        render::ProgramMemoryHints program_memory;
        bool enable_validation{};
    };

    class UiVulkanPresentation final :
        public lux::scene::RenderRuntime, public lux::editor::workbench::SceneViewRenderPort
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<UiVulkanPresentation>, UiVulkanPresentationFailure>;

        [[nodiscard]] static CreateResult create(
            window::LuxWindow& window,
            ui::UISession& session,
            UiVulkanPresentationConfig config
        ) noexcept;

        ~UiVulkanPresentation() noexcept;
        UiVulkanPresentation(const UiVulkanPresentation&) = delete;
        UiVulkanPresentation& operator=(const UiVulkanPresentation&) = delete;

        [[nodiscard]] lux::cxx::expected<void, UiVulkanPresentationFailure> present(ui::UISession& session) noexcept;
        void requestStop() noexcept;
        [[nodiscard]] bool join() noexcept;
        [[nodiscard]] bool stopping() const noexcept;
        [[nodiscard]] lux::cxx::expected<lux::scene::RenderRuntimeLease, lux::scene::RenderRuntimeFailure>
        acquire() noexcept override;
        void pump() override;
        [[nodiscard]] bool framePending() const noexcept override;
        void deferNewFrames(bool defer) noexcept override;
        workbench::SceneViewDiagnostics diagnostics() const noexcept override;
        void drainViewFrames() override;
        lux::scene::RenderRuntime& runtime() noexcept override { return *this; }
        void setViewFrame(const lux::editor::workbench::SceneViewFrame& frame) noexcept override;

    private:
        void release() noexcept override;
        render::RenderControlSession& control() noexcept override;
        render::RenderProgramSession& programs() noexcept override;
        render::RenderUploadClient upload() noexcept override;
        const render::FeatureCatalog& features() const noexcept override;
        struct Impl;
        explicit UiVulkanPresentation(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::application::detail
