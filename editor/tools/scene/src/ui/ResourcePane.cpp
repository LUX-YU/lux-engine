#include <lux/engine/editor/AssetOpenRequest.hpp>
#include <lux/engine/editor/detail/SignalDelivery.hpp>
#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/project/ProjectCatalogAccess.hpp>
#include <lux/engine/editor/ui/scene/ResourcePane.hpp>
#include <lux/engine/window/FileDialog.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <random>

namespace lux::editor::ui
{
    ResourceElement::ResourceElement(
        lux::ui::Pane& parent,
        scene::SceneEditor::Impl& editor,
        assets::AssetImporter& importer,
        EditorResult<void>& status
    )
        : lux::ui::Element(parent, lux::ui::ElementId{"resources"}), importer_(importer),
          resource_connection_(lux::editor::detail::takeConnection(
              lux::object::LuxObject::connect(
                  editor.editor,
                  &scene::SceneEditor::resourcesChanged,
                  [this](std::uint64_t) noexcept { dirty_ = true; }
              ),
              status
          )),
          catalog_connection_(lux::editor::detail::takeConnection(
              lux::object::LuxObject::connect(
                  std::addressof(editor.project()),
                  &ProjectStorage::catalogChanged,
                  [this](std::uint64_t) noexcept { catalog_dirty_ = true; }
              ),
              status
          )),
          editor_(editor)
    {}

    namespace
    {
        std::string_view relativePath(std::string_view path) noexcept
        {
            while (path.starts_with('/'))
            {
                path.remove_prefix(1);
            }
            return path;
        }

        bool containsText(std::string_view text, std::string_view query) noexcept
        {
            const auto fold = [](unsigned char value) {
                return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value - 'A' + 'a') : value;
            };
            return std::search(
                       text.begin(),
                       text.end(),
                       query.begin(),
                       query.end(),
                       [&](unsigned char first, unsigned char second) { return fold(first) == fold(second); }
                   ) != text.end();
        }

        const char* assetType(std::uint32_t magic) noexcept
        {
            if (magic == asset::ModelAsset::primary_magic)
            {
                return "Model";
            }
            if (magic == asset::MeshAsset::primary_magic)
            {
                return "Mesh";
            }
            if (magic == asset::MaterialAsset::primary_magic)
            {
                return "Material";
            }
            if (magic == asset::TextureAsset::primary_magic)
            {
                return "Texture";
            }
            return magic ? "Compiled" : "Source";
        }
    } // namespace

    void ResourceElement::filterCatalog()
    {
        const auto& project = editor_.project();
        const auto assets = project.catalog();
        visible_assets_.clear();
        directories_.clear();
        for (std::size_t index{}; index < assets.size(); ++index)
        {
            const auto& asset = assets[index];
            const auto path = relativePath(asset.path);
            if (!directory_.empty() &&
                (!path.starts_with(directory_) || path.size() <= directory_.size() || path[directory_.size()] != '/'))
            {
                continue;
            }
            const auto local = path.substr(directory_.empty() ? 0 : directory_.size() + 1);
            const auto separator = local.find('/');
            if (separator != std::string_view::npos)
            {
                directories_.emplace_back(local.substr(0, separator));
            }
            const bool matches_type = !type_filter_ || asset.magic == type_filter_;
            const bool matches_text = search_.empty() || containsText(path, search_);
            const bool direct_child = separator == std::string_view::npos;
            if (matches_type && matches_text && (direct_child || !search_.empty()))
            {
                visible_assets_.push_back(index);
            }
        }
        std::ranges::sort(directories_);
        const auto duplicate = std::ranges::unique(directories_);
        directories_.erase(duplicate.begin(), duplicate.end());
        catalog_revision_ = project.catalogRevision();
        catalog_dirty_ = false;
    }

    void ResourceElement::drawCatalog()
    {
        const auto& project = editor_.project();
        if (ImGui::Button("Project"))
        {
            directory_.clear();
            catalog_dirty_ = true;
        }
        if (!directory_.empty())
        {
            ImGui::SameLine();
            if (ImGui::Button("Up"))
            {
                const auto parent = directory_.rfind('/');
                directory_.erase(parent == std::string::npos ? 0 : parent);
                catalog_dirty_ = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(directory_.c_str());
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##asset-search", "Search assets", &search_))
        {
            catalog_dirty_ = true;
        }
        if (ImGui::BeginCombo("Type", type_filter_ ? assetType(type_filter_) : "All"))
        {
            constexpr std::array types{
                0U,
                asset::ModelAsset::primary_magic,
                asset::MeshAsset::primary_magic,
                asset::MaterialAsset::primary_magic,
                asset::TextureAsset::primary_magic
            };
            for (const auto type : types)
            {
                if (ImGui::Selectable(type ? assetType(type) : "All", type == type_filter_))
                {
                    type_filter_ = type;
                    catalog_dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        if (catalog_dirty_ || catalog_revision_ != project.catalogRevision())
        {
            filterCatalog();
        }

        const auto rows = project.catalog();
        if (ImGui::BeginChild("asset-list", {0, 210}, ImGuiChildFlags_Borders))
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(directories_.size() + visible_assets_.size()));
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    ImGui::PushID(row);
                    if (static_cast<std::size_t>(row) < directories_.size())
                    {
                        const auto& name = directories_[row];
                        if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            directory_ += (directory_.empty() ? "" : "/") + name;
                            catalog_dirty_ = true;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("[Folder]");
                    }
                    else
                    {
                        const auto& asset = rows[visible_assets_[static_cast<std::size_t>(row) - directories_.size()]];
                        const auto path = relativePath(asset.path);
                        const auto separator = path.rfind('/');
                        const auto name = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
                        if (ImGui::Selectable(
                                name.data(),
                                selected_ == asset.id,
                                ImGuiSelectableFlags_AllowDoubleClick
                            ))
                        {
                            selected_ = asset.id;
                            if (!asset.source.isNull() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                            {
                                open_requested_ = asset.id;
                            }
                        }
                        if (ImGui::BeginDragDropSource())
                        {
                            const auto reference = project.reference(asset.id);
                            ImGui::SetDragDropPayload(project::kAssetReferencePayload, &reference, sizeof(reference));
                            ImGui::TextUnformatted(asset.path.c_str());
                            ImGui::EndDragDropSource();
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("[%s]", assetType(asset.magic));
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetTooltip("%s", asset.path.c_str());
                        }
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        if (const auto* selected = project.catalogAsset(selected_))
        {
            ImGui::TextWrapped("%s", selected->path.c_str());
        }
    }

    void ResourceElement::update() noexcept
    {
        import_request_ = importer_.currentRequest().value_or(assets::AssetImportId{});
        if (!open_requested_.isNull())
        {
            AssetOpenRequest request{std::exchange(open_requested_, {})};
            static_cast<void>(object::routeEvent(*this, root(), request));
        }
        if (std::exchange(browse_requested_, false))
        {
            const auto result = window::openFileDialog(root().window());
            if (!result)
                import_message_ = result.error().detail;
            else if (*result)
            {
                const auto utf8 = (**result).u8string();
                import_file_.assign(utf8.begin(), utf8.end());
                import_message_.clear();
            }
        }
    }

    void ResourceElement::drawImport()
    {
        if (import_request_.serial)
        {
            const auto state = importer_.status(import_request_);
            if (!state)
            {
                import_message_ = state.error().domain + ": " + state.error().message;
            }
            else if (const auto* pending = std::get_if<assets::AssetImportPending>(&*state))
            {
                constexpr const char* stages[]{
                    "Reading source files",
                    "Cooking model",
                    "Waiting to publish",
                    "Publishing project",
                    "Finishing cancellation"
                };
                ImGui::Text(
                    "%s: %zu files, %zu bytes",
                    stages[static_cast<unsigned>(pending->stage)],
                    pending->files,
                    pending->bytes
                );
                if (ImGui::SmallButton("Cancel import"))
                {
                    static_cast<void>(importer_.abandon(import_request_));
                }
            }
            else if (const auto* error = std::get_if<EditorFailure>(&*state))
            {
                ImGui::TextWrapped("%s (%llu): %s", error->domain.c_str(), error->reason, error->message.c_str());
                if (ImGui::SmallButton("Retry import"))
                {
                    static_cast<void>(importer_.retry(import_request_));
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Abandon import"))
                {
                    static_cast<void>(importer_.abandon(import_request_));
                }
            }
            else
            {
                if (const auto* done = std::get_if<assets::AssetImportSucceeded>(&*state))
                {
                    selected_ = done->asset;
                    import_message_ =
                        done->cleanup ? "Model imported"
                                      : "Model imported; cleanup requires attention: " + done->cleanup.error().message;
                }
                else
                {
                    import_message_ = "Import cancelled";
                }
                if (importer_.acknowledge(import_request_))
                {
                    import_request_ = {};
                }
            }
        }
        if (!import_message_.empty())
        {
            ImGui::TextWrapped("%s", import_message_.c_str());
        }
        if (!ImGui::CollapsingHeader("Import / reimport model"))
        {
            return;
        }
        ImGui::BeginDisabled(!editor_.project().writable() || import_request_.serial != 0);
        ImGui::InputTextWithHint("Source file", "Absolute model file path", &import_file_);
        ImGui::SameLine();
        if (ImGui::Button("Browse..."))
        {
            browse_requested_ = true;
        }
        ImGui::InputText("Destination", &import_destination_);
        ImGui::InputFloat("Scale", &import_config_.uniform_scale);
        ImGui::Checkbox("Left handed", &import_config_.make_left_handed);
        ImGui::SameLine();
        ImGui::Checkbox("Animations", &import_config_.import_animations);
        const auto source = [&] {
            return std::filesystem::path(std::u8string(import_file_.begin(), import_file_.end()));
        };
        const auto accept = [&](EditorResult<assets::AssetImportId> result) {
            if (result)
            {
                import_request_ = *result;
                import_message_.clear();
            }
            else
            {
                import_message_ = result.error().domain + ": " + result.error().message;
            }
        };
        if (ImGui::Button("Import model"))
        {
            std::random_device entropy;
            std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
            std::mt19937 random(seed);
            uuids::uuid_random_generator generate(random);
            accept(importer_.requestModel({asset::AssetId(generate()), source(), import_destination_, import_config_}));
        }
        const auto* selected = editor_.project().catalogAsset(selected_);
        const auto* entry = selected ? editor_.project().asset(selected->source) : nullptr;
        if (entry && entry->kind == EProjectAssetKind::MODEL)
        {
            ImGui::SameLine();
            if (ImGui::Button("Reimport saved source"))
            {
                accept(importer_.reimportModel(entry->id));
            }
            if (ImGui::Button("Reimport from source file"))
            {
                accept(importer_.reimportModel(entry->id, source()));
            }
        }
        ImGui::EndDisabled();
    }

    void ResourceElement::draw() noexcept
    {
        if (ImGui::BeginChild("resource-content", {rect().size.width, rect().size.height}))
            drawResources();
        ImGui::EndChild();
    }

    void ResourceElement::drawResources() noexcept
    {

        if (dirty_)
        {
            snapshot_ = editor_.resources();
            dirty_ = false;
        }
        ImGui::TextDisabled("%s", "Project assets");
        drawCatalog();
        drawImport();
        if (snapshot_)
        {
            constexpr const char*
                states[]{"Unreferenced", "Reading", "Uploading", "Ready", "Failed", "Cancelled", "Capacity"};
            for (const auto& row : snapshot_->rows)
            {
                {
                    const auto& message_value = editor_.project().assetName(row.key.mesh);
                    const std::string_view message{message_value};
                    ImGui::TextUnformatted(
                        message.empty() ? "" : message.data(),
                        message.empty() ? "" : message.data() + message.size()
                    );
                }
                ImGui::SameLine();
                {
                    const auto& message_value = states[static_cast<std::size_t>(row.state)];
                    const std::string_view message{message_value};
                    ImGui::TextDisabled(
                        "%.*s",
                        static_cast<int>(message.size()),
                        message.empty() ? "" : message.data()
                    );
                }
                if (row.state == lux::scene::ERenderAssetState::FAILED)
                {
                    {
                        const auto& message_value =
                            "Dependency: " + std::string(editor_.project().assetName(row.failed_dependency));
                        const std::string_view message{message_value};
                        ImGui::TextUnformatted(
                            message.empty() ? "" : message.data(),
                            message.empty() ? "" : message.data() + message.size()
                        );
                    }
                    std::visit(
                        [](const auto& failure) {
                            using Failure = std::remove_cvref_t<decltype(failure)>;
                            if constexpr (std::same_as<Failure, lux::process::asset_loading::AssetLoadFailure>)
                            {
                                {
                                    const auto& message_value =
                                        "Asset load " + std::to_string(static_cast<unsigned>(failure.code)) +
                                        ", storage " + std::to_string(static_cast<unsigned>(failure.storage_error)) +
                                        ", decode " + std::to_string(static_cast<unsigned>(failure.decode.code));
                                    const std::string_view message{message_value};
                                    ImGui::TextUnformatted(
                                        message.empty() ? "" : message.data(),
                                        message.empty() ? "" : message.data() + message.size()
                                    );
                                }
                            }
                            else if constexpr (std::same_as<Failure, lux::render::RendererFailure>)
                            {
                                ImGui::Text("Render resource admission %u", static_cast<unsigned>(failure.code));
                            }
                            else if constexpr (!std::same_as<Failure, std::monostate>)
                            {
                                {
                                    const auto& message_value =
                                        "Request admission " + std::to_string(static_cast<unsigned>(failure));
                                    const std::string_view message{message_value};
                                    ImGui::TextUnformatted(
                                        message.empty() ? "" : message.data(),
                                        message.empty() ? "" : message.data() + message.size()
                                    );
                                }
                            }
                        },
                        row.failure
                    );
                    ImGui::PushID(static_cast<int>(row.key.sequence));
                    if (ImGui::SmallButton("Retry"))
                    {
                        const auto retried = editor_.retryResource(row.key);
                        action_error_ =
                            retried ? ""
                                    : "Retry rejected: " + std::to_string(static_cast<unsigned>(retried.error().code));
                    }
                    ImGui::PopID();
                }
            }
        }
        if (const auto error = editor_.diagnostic(); !error.empty())
        {
            {
                const auto& message_value = error;
                const std::string_view message{message_value};
                ImGui::TextUnformatted(
                    message.empty() ? "" : message.data(),
                    message.empty() ? "" : message.data() + message.size()
                );
            }
        }
        if (!action_error_.empty())
        {
            {
                const auto& message_value = action_error_;
                const std::string_view message{message_value};
                ImGui::TextUnformatted(
                    message.empty() ? "" : message.data(),
                    message.empty() ? "" : message.data() + message.size()
                );
            }
        }
    }
} // namespace lux::editor::ui
