#pragma once
#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <lux/engine/ui/Controls.hpp>

namespace lux::editor::project
{
    class AssetPickerElement final : public lux::ui::Element
    {
    public:
        object::TSignal<lux::ui::EditResult> edited{*this};
        AssetPickerElement(
            lux::ui::Element&,
            lux::ui::ElementId,
            ProjectCatalogAccess,
            std::uint32_t required_magic,
            asset::AssetId value = {}
        );
        AssetPickerElement(const AssetPickerElement&) = delete;
        AssetPickerElement& operator=(const AssetPickerElement&) = delete;
        AssetPickerElement(AssetPickerElement&&) = delete;
        AssetPickerElement& operator=(AssetPickerElement&&) = delete;
        void setValue(asset::AssetId) noexcept;
        [[nodiscard]] asset::AssetId value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] ProjectQueryResult<void> select(AssetReference);
        [[nodiscard]] ProjectQueryResult<void> refresh();
        [[nodiscard]] const std::optional<VProjectQueryFailure>& error() const noexcept
        {
            return error_;
        }
        [[nodiscard]] object::SignalDelivery delivery() const noexcept
        {
            return delivery_;
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        void update() noexcept override;
        void adopt(asset::AssetId) noexcept;
        ProjectCatalogAccess query_;
        ProjectCatalog catalog_;
        std::uint32_t required_magic_;
        asset::AssetId value_;
        std::optional<VProjectQueryFailure> error_;
        object::SignalDelivery delivery_;
    };
}
