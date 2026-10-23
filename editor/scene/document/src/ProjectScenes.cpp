#include <algorithm>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/ProjectScenes.hpp>

namespace lux::editor
{
    ProjectScenes::ProjectScenes(EditorContext& context) noexcept : context_(context) {}

    ProjectScenes::~ProjectScenes() noexcept = default;

    ProjectSceneResult<SceneDocument*> ProjectScenes::adopt(
        std::unique_ptr<SceneDocument>& document,
        std::filesystem::path path
    ) noexcept
    {
        const bool invalid_document = !document || path.empty();
        if (invalid_document)
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::INVALID_DOCUMENT});
        }
        const auto id = document->id();
        if (documents_.contains(id))
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::DUPLICATE_DOCUMENT});
        }
        auto* result = document.get();
        documents_.emplace(id, Entry{std::move(document), std::move(path)});
        return result;
    }

    SceneDocument* ProjectScenes::find(asset::AssetId id) noexcept
    {
        const auto found = documents_.find(id);
        return found == documents_.end() || found->second.retirement ? nullptr : found->second.document.get();
    }

    const std::filesystem::path* ProjectScenes::path(asset::AssetId id) const noexcept
    {
        const auto found = documents_.find(id);
        return found == documents_.end() ? nullptr : &found->second.path;
    }

    std::vector<asset::AssetId> ProjectScenes::documents() const
    {
        std::vector<asset::AssetId> result;
        result.reserve(documents_.size());
        for (const auto& [id, entry] : documents_)
        {
            if (!entry.retirement)
            {
                result.push_back(id);
            }
        }
        return result;
    }

    ProjectSceneResult<void> ProjectScenes::close(asset::AssetId id) noexcept
    {
        const auto found = documents_.find(id);
        if (found == documents_.end())
        {
            return cxx::unexpected(VProjectSceneFailure{EProjectSceneError::NOT_OPEN});
        }
        if (found->second.retirement)
        {
            return {};
        }
        auto retired = context_.retireScene(found->second.document->instance());
        if (!retired)
        {
            return cxx::unexpected(VProjectSceneFailure{retired.error()});
        }
        found->second.document->setReadOnly(true);
        found->second.retirement = std::move(*retired);
        return {};
    }

    void ProjectScenes::collectClosed() noexcept
    {
        std::erase_if(
            documents_,
            [](const auto& item) noexcept { return item.second.retirement && item.second.retirement->complete(); }
        );
    }
} // namespace lux::editor
