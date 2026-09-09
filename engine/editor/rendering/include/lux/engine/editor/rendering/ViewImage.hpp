#pragma once
#include <lux/engine/editor/rendering/RendererConfig.hpp>
namespace lux::editor::rendering
{
    namespace detail
    {
        struct ViewImageAccess;
        class ViewResources;
    } // namespace detail
    class EditorRenderer;
    class RenderView;
    class EditorFramePacket;
    class LUX_EDITOR_RENDERING_PUBLIC ViewImageLease final
    {
      public:
        ViewImageLease() noexcept;
        ~ViewImageLease() noexcept;
        ViewImageLease(const ViewImageLease &) noexcept;
        ViewImageLease &operator=(const ViewImageLease &) noexcept;
        ViewImageLease(ViewImageLease &&) noexcept;
        ViewImageLease &operator=(ViewImageLease &&) noexcept;
        [[nodiscard]] bool valid() const noexcept;

      private:
        friend class EditorRenderer;
        friend class RenderView;
        friend class EditorFramePacket;
        friend struct detail::ViewImageAccess;
        struct Record;
        std::shared_ptr<const Record> record_;
    };
    struct ViewImage final
    {
        lux::ui::TextureHandle texture;
        PixelExtent extent;
        RenderViewId view;
        ImageContentStamp content;
        ViewImageLease lease;
    };
    struct ViewStatus final
    {
        EViewState state{};
        RenderViewId view;
        PixelExtent requested_extent{}, ready_extent{};
        std::uint64_t request_sequence{}, acknowledged_sequence{};
        std::optional<RendererFailure> failure;
    };
} // namespace lux::editor::rendering
