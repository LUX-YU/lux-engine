#pragma once

#include <lux/engine/editor/EditorError.hpp>
#include <lux/engine/editor/storage/AssetCatalog.hpp>
#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/ui/Controls.hpp>

namespace lux::editor
{
    class ProjectStorage;
}

namespace lux::editor::ui
{
    // Display and selection only. The receiver commits changes to its own model/history.
    class LUX_EDITOR_UI_PUBLIC AssetPickerElement final : public lux::ui::Element
    {
    public:
        object::TSignal<lux::ui::EditResult> edited{*this};
        AssetPickerElement(
            lux::ui::Element&,
            lux::ui::ElementId,
            const ProjectStorage*,
            std::uint32_t required_magic,
            asset::AssetId value = {}
        );
        void setValue(asset::AssetId) noexcept;
        [[nodiscard]] asset::AssetId value() const noexcept
        {
            return value_;
        }
        [[nodiscard]] EditorResult<void> select(AssetReference) noexcept;
        [[nodiscard]] const std::optional<EditorFailure>& error() const noexcept
        {
            return error_;
        }

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override;
        void draw() noexcept override;
        void adopt(asset::AssetId) noexcept;
        const ProjectStorage* project_;
        std::uint32_t required_magic_;
        asset::AssetId value_;
        std::optional<EditorFailure> error_;
    };
}
