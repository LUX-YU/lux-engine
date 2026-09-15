#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/gui/asset/AssetReference.hpp>
#include <lux/engine/editor/gui/scene/ResourcePane.hpp>
#include <lux/engine/editor/gui/shell/EditorWindow.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <random>

namespace lux::editor::gui
{
ResourcePane::ResourcePane(scene::SceneEditor &document, EditorWindow &window, process::ExecutionRuntime &runtime,
                           std::string id)
    : DocumentPane(document, std::move(id), "Resources"), importer_(document.project(), runtime),
      resource_connection_(document.observeScoped<scene::SceneEditor::resourcesChanged>(
          [this](std::uint64_t) noexcept { dirty_ = true; })),
      catalog_connection_(document.project().observeScoped<Project::catalogChanged>(
          [this](std::uint64_t) noexcept { catalog_dirty_ = true; })),
      window_(window)
{
}

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
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
                       [&](unsigned char first, unsigned char second) { return fold(first) == fold(second); }) !=
           text.end();
}

const char *assetType(std::uint32_t magic) noexcept
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

void ResourcePane::filterCatalog()
{
    const auto &project = document_.project();
    const auto assets = project.catalog();
    visible_assets_.clear();
    directories_.clear();
    for (std::size_t index{}; index < assets.size(); ++index)
    {
        const auto &asset = assets[index];
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

void ResourcePane::drawCatalog()
{
    const auto &project = document_.project();
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
        constexpr std::array types{0U, asset::ModelAsset::primary_magic, asset::MeshAsset::primary_magic,
                                   asset::MaterialAsset::primary_magic, asset::TextureAsset::primary_magic};
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
                    const auto &name = directories_[row];
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
                    const auto &asset = rows[visible_assets_[static_cast<std::size_t>(row) - directories_.size()]];
                    const auto path = relativePath(asset.path);
                    const auto separator = path.rfind('/');
                    const auto name = path.substr(separator == std::string_view::npos ? 0 : separator + 1);
                    if (ImGui::Selectable(name.data(), selected_ == asset.id, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selected_ = asset.id;
                        if (!asset.source.isNull() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        {
                            window_.openAsset(asset.source);
                        }
                    }
                    if (ImGui::BeginDragDropSource())
                    {
                        const auto reference = project.reference(asset.id);
                        ImGui::SetDragDropPayload(kAssetReferencePayload, &reference, sizeof(reference));
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
    if (const auto *selected = project.catalogAsset(selected_))
    {
        ImGui::TextWrapped("%s", selected->path.c_str());
    }
}

void ResourcePane::poll(PollBudget &budget)
{
    DocumentPane::poll(budget);
    importer_.poll(budget);
    if (close_requested_ && importer_.closeStatus().state == ECloseState::CLOSED)
    {
        DocumentPane::requestClose();
    }
}

void ResourcePane::requestClose() noexcept
{
    close_requested_ = true;
    importer_.requestClose();
    if (importer_.closeStatus().state == ECloseState::CLOSED)
    {
        DocumentPane::requestClose();
    }
}

CloseStatus ResourcePane::closeStatus() const
{
    return close_requested_ ? importer_.closeStatus() : DocumentPane::closeStatus();
}

void ResourcePane::drawImport()
{
    if (import_request_.serial)
    {
        const auto state = importer_.status(import_request_);
        if (!state)
        {
            import_message_ = state.error().domain + ": " + state.error().message;
        }
        else if (const auto *pending = std::get_if<assets::AssetImportPending>(&*state))
        {
            constexpr const char *stages[]{"Reading source files", "Cooking model", "Waiting to publish",
                                           "Publishing project", "Finishing cancellation"};
            ImGui::Text("%s: %zu files, %zu bytes", stages[static_cast<unsigned>(pending->stage)], pending->files,
                        pending->bytes);
            if (ImGui::SmallButton("Cancel import"))
            {
                static_cast<void>(importer_.abandon(import_request_));
            }
        }
        else if (const auto *error = std::get_if<EditorFailure>(&*state))
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
            if (const auto *done = std::get_if<assets::AssetImportSucceeded>(&*state))
            {
                selected_ = done->asset;
                import_message_ = done->cleanup
                                      ? "Model imported"
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
    ImGui::BeginDisabled(!document_.project().writable() || import_request_.serial != 0 || close_requested_);
    ImGui::InputTextWithHint("Source file", "Absolute model file path", &import_file_);
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        auto path = std::filesystem::u8path(import_file_);
        const auto selected = window_.selectExistingFile(path);
        if (!selected)
        {
            import_message_ = "File selection failed: " + std::to_string(selected.error().request);
        }
        else if (*selected)
        {
            const auto utf8 = path.u8string();
            import_file_.assign(utf8.begin(), utf8.end());
            import_message_.clear();
        }
    }
    ImGui::InputText("Destination", &import_destination_);
    ImGui::InputFloat("Scale", &import_config_.uniform_scale);
    ImGui::Checkbox("Left handed", &import_config_.make_left_handed);
    ImGui::SameLine();
    ImGui::Checkbox("Animations", &import_config_.import_animations);
    const auto source = [&] { return std::filesystem::path(std::u8string(import_file_.begin(), import_file_.end())); };
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
    const auto *selected = document_.project().catalogAsset(selected_);
    const auto *entry = selected ? document_.project().asset(selected->source) : nullptr;
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

void ResourcePane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
{
    context.activateContext(lux::ui::UiContextIdView{id()});
    if (close_requested_)
    {
        frame.textMuted("Finishing model import before closing");
        drawImport();
        return;
    }
    if (dirty_)
    {
        snapshot_ = document_.resources();
        dirty_ = false;
    }
    frame.textMuted("Project assets");
    drawCatalog();
    drawImport();
    if (snapshot_)
    {
        constexpr const char *states[]{"Unreferenced", "Reading",    "Uploading", "Ready",   "Failed",
                                       "Cancelled",    "Superseded", "Releasing", "Released"};
        for (const auto &row : snapshot_->rows)
        {
            frame.text(document_.project().assetName(row.key.mesh));
            ImGui::SameLine();
            frame.textMuted(states[static_cast<std::size_t>(row.state)]);
            if (row.state == scene::ESceneResourceState::FAILED)
            {
                frame.text("Dependency: " + std::string(document_.project().assetName(row.failed_dependency)));
                std::visit(
                    [&frame](const auto &failure) {
                        using Failure = std::remove_cvref_t<decltype(failure)>;
                        if constexpr (std::same_as<Failure, lux::process::asset_loading::AssetLoadFailure>)
                        {
                            frame.text("Asset load " + std::to_string(static_cast<unsigned>(failure.code)) +
                                       ", storage " + std::to_string(static_cast<unsigned>(failure.storage_error)) +
                                       ", decode " + std::to_string(static_cast<unsigned>(failure.decode.code)));
                        }
                        else if constexpr (!std::same_as<Failure, std::monostate>)
                        {
                            frame.text("Request admission " + std::to_string(static_cast<unsigned>(failure)));
                        }
                    },
                    row.failure);
                ImGui::PushID(static_cast<int>(row.key.sequence));
                if (frame.smallButton("Retry"))
                {
                    const auto retried = document_.retryResource(row.key);
                    action_error_ =
                        retried ? "" : "Retry rejected: " + std::to_string(static_cast<unsigned>(retried.error().code));
                }
                ImGui::PopID();
            }
        }
    }
    if (const auto error = document_.diagnostic(); !error.empty())
    {
        frame.text(error);
    }
    if (!action_error_.empty())
    {
        frame.text(action_error_);
    }
}
} // namespace lux::editor::gui
