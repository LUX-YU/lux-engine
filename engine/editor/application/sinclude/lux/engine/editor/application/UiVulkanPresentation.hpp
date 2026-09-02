#pragma once

#include <lux/engine/function/render/client/core/RenderError.hpp>
#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/engine/ui/detail/UiVulkanBackend.hpp>

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

    class UiVulkanPresentation final
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

    private:
        struct Impl;
        explicit UiVulkanPresentation(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::application::detail
