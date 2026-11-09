#include <imgui.h>
#include <lux/engine/editor/AssetBrowserPane.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Root.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::editor
{
    struct AssetBrowserPane::Impl final
    {
        class AssetList final : public ui::Element
        {
        public:
            struct Row final
            {
                std::string label, imgui_id;
                AssetReference reference;
            };

            std::vector<Row> rows;
            asset::AssetId selected;

        private:
            ui::SizeHint sizeHintContent() noexcept override
            {
                ui::SizeHint hint;
                hint.minimum = {100, 80};
                hint.preferred = {300, 260};
                return hint;
            }

            void draw() noexcept override
            {
                ImGui::BeginChild("assets", {rect().size.width, rect().size.height});
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(rows.size()));
                while (clipper.Step())
                {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                    {
                        const auto& row = rows[i];
                        const asset::AssetId id{row.reference.id};
                        ImGui::PushID(row.imgui_id.c_str());
                        if (ImGui::Selectable(row.label.c_str(), selected == id))
                        {
                            selected = id;
                        }
                        if (ImGui::BeginDragDropSource())
                        {
                            ui::setDragDropPayload(
                                ui::PayloadTypeIdView{kAssetPayload},
                                std::as_bytes(std::span(&row.reference, 1))
                            );
                            ImGui::TextUnformatted(row.label.c_str());
                            ImGui::EndDragDropSource();
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
        };

        ProjectAssets& assets;
        ui::Layout layout;
        ui::TextEdit source;
        ui::Choice importer_choice{{}};
        ui::Layout configuration;
        std::vector<AssetImportEditor> editors;
        std::unique_ptr<ui::Element> draft;
        const AssetImportEditor* editor = nullptr;
        ui::Button import{"Import asset"}, reimport{"Reimport selected"}, reload{"Reload catalog"};
        ui::Label status;
        AssetList list;
        std::vector<object::Connection> connections;

        Impl(ProjectAssets& assets, std::vector<AssetImportEditor> editors) noexcept
            : assets(assets), editors(std::move(editors))
        {
            source.setHint("Absolute source path");
            std::vector<ui::ChoiceOption> choices;
            for (std::size_t i = 0; i < assets.importers().size(); ++i)
            {
                choices.push_back({static_cast<std::int64_t>(i), assets.importers()[i].name});
            }
            importer_choice.setOptions(std::move(choices));
            const auto add = [&](ui::Element& value)
            {
                if (!layout.addElement(value))
                {
                    std::terminate();
                }
            };
            add(importer_choice);
            add(source);
            add(configuration);
            add(import);
            add(reimport);
            add(reload);
            add(status);
            add(list);
            for (auto* control : std::array<ui::Element*, 5>{&source, &import, &reimport, &reload, &status})
            {
                control->setStretch({1, 0});
            }
        }
    };

    AssetBrowserPane::AssetBrowserPane(ProjectAssets& assets, std::vector<AssetImportEditor> editors, std::string title)
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(assets, std::move(editors)))
    {
        if (!addElement(impl_->layout))
        {
            std::terminate();
        }
        const auto retain = [&](auto connection)
        {
            if (!connection)
            {
                std::terminate();
            }
            impl_->connections.push_back(std::move(*connection));
        };
        retain(connect(&assets, &ProjectAssets::changed, this, [this]() noexcept { refresh(); }));
        retain(connect(
            &impl_->import,
            &ui::Button::activated,
            this,
            [this]() noexcept
            {
                const auto index = impl_->importer_choice.value();
                if (index >= impl_->assets.importers().size() || !impl_->draft || !impl_->editor)
                {
                    return;
                }
                auto bytes = impl_->editor->encode(*impl_->draft);
                if (!bytes)
                {
                    impl_->status.setText(bytes.error().detail);
                    return;
                }
                const auto& provider = impl_->assets.importers()[index];
                auto result = impl_->assets.importAsset(
                    provider.name,
                    provider.version,
                    std::filesystem::u8path(impl_->source.value()),
                    std::move(*bytes)
                );
                impl_->status.setText(result ? "Importing..." : result.error().detail);
            }
        ));
        retain(connect(
            &impl_->reimport,
            &ui::Button::activated,
            this,
            [this]() noexcept
            {
                auto result = impl_->assets.reimport(impl_->list.selected);
                impl_->status.setText(result ? "Reimporting..." : result.error().detail);
            }
        ));
        retain(connect(
            &impl_->reload,
            &ui::Button::activated,
            this,
            [this]() noexcept
            {
                auto result = impl_->assets.open();
                impl_->status.setText(result ? "Opening catalog..." : result.error().detail);
            }
        ));
        retain(connect(
            &impl_->importer_choice,
            &ui::Choice::edited,
            this,
            [this](auto) noexcept
            {
                root().deferChange(
                    *this,
                    [](object::LuxObject& value) noexcept { static_cast<AssetBrowserPane&>(value).selectImporter(); }
                );
            }
        ));
        selectImporter();
        refresh();
    }

    void AssetBrowserPane::selectImporter() noexcept
    {
        impl_->draft.reset();
        impl_->editor = nullptr;
        impl_->import.setEnabled(false);
        const auto index = impl_->importer_choice.value();
        if (index >= impl_->assets.importers().size())
        {
            return;
        }
        const auto& provider = impl_->assets.importers()[index];
        const auto compatible = [&](const AssetImportEditor& editor) noexcept
        {
            return editor.importer == provider.name && editor.version == provider.version &&
                   editor.type == provider.configuration_type;
        };
        const auto found = std::ranges::find_if(impl_->editors, compatible);
        if (found == impl_->editors.end() || std::ranges::count_if(impl_->editors, compatible) != 1)
        {
            impl_->status.setText("Configuration editor unavailable; stored bytes remain unchanged");
            return;
        }
        auto draft = found->create(provider.default_configuration);
        if (!draft)
        {
            impl_->status.setText(draft.error().detail);
            return;
        }
        impl_->draft = std::move(*draft);
        impl_->editor = &*found;
        impl_->import.setEnabled(true);
        if (!impl_->configuration.addElement(*impl_->draft))
        {
            std::terminate();
        }
    }

    AssetBrowserPane::~AssetBrowserPane() noexcept = default;

    void AssetBrowserPane::refresh() noexcept
    {
        impl_->list.rows.clear();
        for (const auto& record : impl_->assets.records())
        {
            AssetReference reference;
            std::memcpy(reference.id.data(), record.id.bytes().data(), reference.id.size());
            reference.revision = record.revision;
            reference.type = record.type;
            impl_->list.rows.push_back(
                {record.name + " v" + std::to_string(record.revision), uuids::to_string(record.id.uuid()), reference}
            );
        }
        impl_->status.setText(
            impl_->assets.failure() ? impl_->assets.failure()->detail
            : !impl_->editor        ? "Importer/configuration editor unavailable; existing records remain unchanged"
            : impl_->assets.ready() ? "Drag an asset to a compatible tool"
                                    : "Opening asset catalog..."
        );
    }
} // namespace lux::editor
