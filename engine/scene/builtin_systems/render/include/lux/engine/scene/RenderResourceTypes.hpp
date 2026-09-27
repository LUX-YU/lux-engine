#pragma once

#include <lux/engine/render/RendererConfig.hpp>
#include <lux/engine/function/render/features/resources/ResourceHandles.hpp>
#include <lux/cxx/container/SlotMap.hpp>

namespace lux::scene
{
    struct RenderResourceTag;
    struct RenderResourceId final
    {
        std::uint64_t domain{};
        lux::cxx::SlotKey<RenderResourceTag> slot;
        [[nodiscard]] bool isValid() const noexcept
        {
            return domain != 0 && slot.isValid();
        }
        friend bool operator==(RenderResourceId, RenderResourceId) = default;
    };

    struct ViewStamp final
    {
        std::uint64_t session{}, source_revision{}, view_revision{}, surface_generation{};
        friend constexpr bool operator==(const ViewStamp&, const ViewStamp&) noexcept = default;
    };
    enum class EImageEvidence : std::uint8_t
    {
        REQUESTED,
        RECORDED,
        SUBMITTED,
        GPU_COMPLETE
    };
    // GPU_COMPLETE is NOT proof of physical screen presentation.
    struct ImageContentStamp final
    {
        ViewStamp source;
        std::uint64_t frame_serial{}; // Zero when no actual recorded frame is known.
        EImageEvidence evidence{EImageEvidence::REQUESTED};
    };
    enum class EViewState : std::uint8_t
    {
        CREATING,
        READY,
        RESIZING,
        SUSPENDED,
        CLOSING,
        CLOSED,
        FAILED
    };
    struct SampledOutput final
    {};
    struct NativeSurfaceOutput final
    {
        std::uint64_t native_window{}; // Platform handle; the owner keeps it alive through target retirement.
    };
    struct ViewConfig final
    {
        render::PixelExtent extent;
        std::variant<SampledOutput, NativeSurfaceOutput> output;
    };
    struct ViewStatus final
    {
        EViewState state{};
        render::PixelExtent requested_extent{}, ready_extent{};
        std::uint64_t request_sequence{}, acknowledged_sequence{};
        std::optional<render::RendererFailure> failure;
    };
    struct ViewObservation final
    {
        render::RenderSceneId scene;
        render::ViewHandle handle;
        ViewStatus status;
        render::PixelExtent render_extent;
        std::uint64_t render_sequence{}; // Bound producer generation; may precede a sampleable output.
    };

    struct RenderOutputInfo final
    {
        render::RTextureHandle texture;
        render::RenderTargetId target;
        render::PixelExtent extent;
        ImageContentStamp content;
    };

}
