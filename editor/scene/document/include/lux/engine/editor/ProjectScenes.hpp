#pragma once

#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/error/Error.hpp>

#include <filesystem>
#include <map>

namespace lux::editor
{
    class EditorContext;
    enum class EProjectSceneError : std::uint8_t
    {
        INVALID_DOCUMENT,
        DUPLICATE_DOCUMENT,
        NOT_OPEN
    };
    using VProjectSceneFailure = std::variant<EProjectSceneError, error::Error>;
    template <class T> using ProjectSceneResult = cxx::expected<T, VProjectSceneFailure>;

    // Unique collection of open author documents. SceneRuntime instances remain leased by EditorContext.
    class ProjectScenes final
    {
    public:
        explicit ProjectScenes(EditorContext&) noexcept;
        ~ProjectScenes() noexcept;
        [[nodiscard]] ProjectSceneResult<SceneDocument*> adopt(
            std::unique_ptr<SceneDocument>&,
            std::filesystem::path
        ) noexcept;
        [[nodiscard]] SceneDocument* find(asset::AssetId) noexcept;
        [[nodiscard]] const std::filesystem::path* path(asset::AssetId) const noexcept;
        [[nodiscard]] std::vector<asset::AssetId> documents() const;
        // UI sampling and view inputs must already be withdrawn. Document storage survives actual retirement.
        [[nodiscard]] ProjectSceneResult<void> close(asset::AssetId) noexcept;
        void collectClosed() noexcept;

    private:
        struct Entry final
        {
            std::unique_ptr<SceneDocument> document;
            std::filesystem::path path;
            std::optional<scene::InstanceRetirement> retirement;
        };

        EditorContext& context_;
        std::map<asset::AssetId, Entry> documents_;
    };
} // namespace lux::editor
