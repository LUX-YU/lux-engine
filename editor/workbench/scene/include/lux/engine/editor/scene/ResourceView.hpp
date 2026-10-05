#pragma once
#include <lux/engine/editor/scene/ResourceStatus.hpp>
#include <lux/engine/editor/views/ViewInfo.hpp>
#include <lux/engine/ui/Pane.hpp>

namespace lux::editor::scene
{
    struct ResourceViewBinding final
    {
        lux::scene::SceneInstanceId instance;
        system::SystemInstanceId system;
        friend bool operator==(ResourceViewBinding, ResourceViewBinding) = default;
    };

    // Resource diagnostics borrow an explicitly selected instance. The Runtime retains all requests,
    // completions and retirement responsibilities when this view disappears.
    class ResourceView final : public lux::ui::Pane
    {
    public:
        ResourceView(object::ObjectDispatcherRef, lux::ui::PaneId, lux::scene::SceneRuntime&);
        ~ResourceView() noexcept override;
        ResourceView(const ResourceView&) = delete;
        ResourceView& operator=(const ResourceView&) = delete;
        ResourceView(ResourceView&&) = delete;
        ResourceView& operator=(ResourceView&&) = delete;
        [[nodiscard]] render::RenderResult<void> rebind(std::optional<ResourceViewBinding>);
        // Validate at the composition boundary, then retain only the original attachment identity.
        // Later reads use this window's Root; a removed/replaced source cannot redirect the binding.
        [[nodiscard]] render::RenderResult<void> followViewport(lux::ui::Root&, lux::ui::PaneHandle);
        [[nodiscard]] views::ViewContent content() const noexcept;
        [[nodiscard]] render::RenderResult<void> refresh();
        [[nodiscard]] render::RenderResult<void> retry(const lux::scene::RenderAssetKey&);
        [[nodiscard]] const ResourceStatusSnapshot& snapshot() const noexcept;
        [[nodiscard]] const render::RenderResult<void>& status() const noexcept;

    private:
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
