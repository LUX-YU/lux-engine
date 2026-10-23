#pragma once

#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/ScenePackageFile.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/SceneViewRequest.hpp>

#include <filesystem>
#include <memory>

namespace lux::editor
{
    class EditorContext;
    class ProjectAssets;
    enum class EProjectSceneError : std::uint8_t
    {
        INVALID_DOCUMENT,
        DUPLICATE_DOCUMENT,
        NOT_OPEN,
        CLOSED,
        RENDER_UNAVAILABLE,
        INVALID_VIEW,
        BUSY
    };

    // A failed catalog publication may leave a complete, unreferenced Scene file. It is not rolled back
    // or reported as cancellation; the user can retry/open that file without losing authored data.
    struct SceneCreationFailure final
    {
        std::filesystem::path scene_path;
        std::variant<SceneFileFailure, ProjectFailure> cause;
        bool scene_published{};
    };

    using VProjectSceneFailure = std::variant<
        EProjectSceneError,
        error::Error,
        SceneFileFailure,
        scene::ScenePackageFailure,
        scene::SceneRuntimeFailure,
        ProjectFailure,
        SceneCreationFailure>;
    template <class T> using ProjectSceneResult = cxx::expected<T, VProjectSceneFailure>;
    [[nodiscard]] std::string describeProjectSceneFailure(const VProjectSceneFailure&);
    struct SceneViewportTag;
    using SceneViewportId = cxx::SlotKey<SceneViewportTag>;

    struct SceneViewportStatus final
    {
        rdesc::UiImageToken image;
        render::TargetId target;
        render::RenderError error;
        bool ready{};
    };

    // One project's open documents and observations. Context leases actual Scene instances;
    // RenderSystem owns Render identities, and this collection retains viewport outputs through withdrawal.
    class ProjectScenes final : public object::LuxObject
    {
    public:
        ProjectScenes(EditorContext&, ProjectAssets&) noexcept;
        ~ProjectScenes() noexcept;
        // Admission creates one existing monitored Task. IO, decode and Main adoption use the shared execution.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> open(std::filesystem::path) noexcept;
        [[nodiscard]] ProjectSceneResult<execution::TaskId> create(
            ProjectManifest::SceneRecord,
            SceneCreateSelection
        ) noexcept;
        [[nodiscard]] SceneDocument* find(asset::AssetId) noexcept;
        [[nodiscard]] const std::filesystem::path* path(asset::AssetId) const noexcept;
        [[nodiscard]] std::vector<asset::AssetId> documents() const;
        [[nodiscard]] ProjectSceneResult<void> close(asset::AssetId) noexcept;
        void closeAll() noexcept;
        // Main safe point, including while closing. No window, CPU or GPU blocking wait.
        void maintain() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] const std::optional<VProjectSceneFailure>& failure() const noexcept;
        // Called after the project publishes a complete VFS version, independently of viewport changes.
        void setAssetRevision(std::uint64_t) noexcept;

        [[nodiscard]] ProjectSceneResult<SceneViewportId> addViewport(
            asset::AssetId,
            scene::VSceneCameraSource,
            std::uint32_t width,
            std::uint32_t height
        ) noexcept;
        [[nodiscard]] bool updateViewport(
            SceneViewportId,
            scene::VSceneCameraSource,
            std::uint32_t width,
            std::uint32_t height
        ) noexcept;
        void closeViewport(SceneViewportId) noexcept;
        [[nodiscard]] SceneViewportStatus viewport(SceneViewportId) const noexcept;

        object::TSignal<asset::AssetId> opened{*this};
        object::TSignal<> changed{*this};

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
