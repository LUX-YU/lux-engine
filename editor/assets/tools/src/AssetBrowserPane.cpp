#include <imgui.h>
#include <lux/engine/editor/AssetBrowserPane.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/DragDrop.hpp>
#include <lux/engine/ui/Layout.hpp>

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
                ModelAssetReference reference;
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
                                ui::PayloadTypeIdView{kModelAssetPayload},
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
        ui::Button import{"Import model"}, reimport{"Reimport selected"}, reload{"Reload catalog"};
        ui::Label status;
        AssetList list;
        std::vector<object::Connection> connections;

        explicit Impl(ProjectAssets& assets) noexcept : assets(assets)
        {
            source.setHint("Absolute model path (.gltf/.obj/...)");
            const auto add = [&](ui::Element& value)
            {
                if (!layout.addElement(value))
                {
                    std::terminate();
                }
            };
            add(source);
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

    AssetBrowserPane::AssetBrowserPane(ProjectAssets& assets, std::string title)
        : Pane(std::move(title)), impl_(std::make_unique<Impl>(assets))
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
                auto result = impl_->assets.importModel(std::filesystem::u8path(impl_->source.value()));
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
        refresh();
    }

    AssetBrowserPane::~AssetBrowserPane() noexcept = default;

    void AssetBrowserPane::refresh() noexcept
    {
        impl_->list.rows.clear();
        for (const auto& record : impl_->assets.records())
        {
            ModelAssetReference reference;
            std::memcpy(reference.id.data(), record.id.bytes().data(), reference.id.size());
            reference.revision = record.revision;
            impl_->list.rows.push_back(
                {record.name + " v" + std::to_string(record.revision), uuids::to_string(record.id.uuid()), reference}
            );
        }
        impl_->status.setText(
            impl_->assets.failure() ? impl_->assets.failure()->detail
            : impl_->assets.ready() ? "Drag a model into a Scene viewport"
                                    : "Opening asset catalog..."
        );
    }
} // namespace lux::editor
