#include <lux/engine/editor/detail/SignalDelivery.hpp>
#pragma once
#include <lux/engine/editor/assets/AssetImporter.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>

namespace lux::editor::ui
{
    class ResourceElement final : public lux::ui::Element
    {
    public:
        ResourceElement(lux::ui::Pane&, scene::SceneEditor::Impl&, assets::AssetImporter&, EditorResult<void>&);
        void update() noexcept override;

    private:
        void draw() noexcept override;
        void drawResources() noexcept;
        void drawCatalog();
        void drawImport();
        assets::AssetImporter& importer_;
        assets::AssetImportId import_request_;
        std::string import_file_;
        std::string import_destination_{"Models/New model"};
        lux::toolchain::ModelCookConfiguration import_config_;
        std::string import_message_;
        void filterCatalog();
        std::shared_ptr<const scene::SceneResourceSnapshot> snapshot_;
        bool dirty_{true};
        bool catalog_dirty_{true};
        std::uint64_t catalog_revision_{};
        std::string directory_;
        std::string search_;
        std::uint32_t type_filter_{};
        std::vector<std::size_t> visible_assets_;
        std::vector<std::string> directories_;
        asset::AssetId selected_;
        std::string action_error_;
        object::Connection resource_connection_;
        object::Connection catalog_connection_;
        scene::SceneEditor::Impl& editor_;
        bool browse_requested_{};
        asset::AssetId open_requested_;
    };
    class ResourcePane final : public lux::ui::Pane
    {
    public:
        ResourcePane(
            scene::SceneEditor::Impl& editor,
            assets::AssetImporter& importer,
            lux::ui::PaneId id,
            EditorResult<void>& status
        )
            : lux::ui::Pane(*editor.editor, std::move(id), lux::ui::PaneTypeId{"lux.editor.resources"}, "Resources"),
              content_(*this, editor, importer, status), close_(lux::editor::detail::takeConnection(
                                                               lux::object::LuxObject::connect(
                                                                   this,
                                                                   &lux::ui::Pane::closeRequested,
                                                                   [this]() noexcept { hide_ = true; }
                                                               ),
                                                               status
                                                           ))
        {
            setContent(content_);
        }
    private:
        void update() noexcept override
        {
            if (std::exchange(hide_, false))
                setVisible(false);
        }
        ResourceElement content_;
        bool hide_{};
        object::Connection close_;
    };
} // namespace lux::editor::ui
