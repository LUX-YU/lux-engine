#pragma once
#include <lux/engine/editor/asset/AssetImporter.hpp>
#include <lux/engine/editor/gui/DocumentPane.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>

namespace lux::editor::gui
{
    class EditorWindow;
    class ResourcePane final : public DocumentPane<ResourcePane, scene::SceneEditor>
    {
      public:
        ResourcePane(scene::SceneEditor &, EditorWindow &, process::ExecutionRuntime &, std::string id);
        void poll(PollBudget &) override;
        void requestClose() noexcept override;
        CloseStatus closeStatus() const override;

      private:
        void draw(lux::ui::Frame &, lux::ui::PaneDrawContext &) override;
        void drawCatalog();
        void drawImport();
        assets::AssetImporter importer_;
        assets::AssetImportId import_request_;
        std::string import_file_;
        std::string import_destination_{"Models/New model"};
        lux::toolchain::ModelCookConfiguration import_config_;
        std::string import_message_;
        bool close_requested_{};
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
        object::ScopedConnection resource_connection_;
        object::ScopedConnection catalog_connection_;
        EditorWindow &window_;
    };
} // namespace lux::editor::gui
