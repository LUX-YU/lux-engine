#pragma once

#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/description/UiDraw.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/ScenePackageFile.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>

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
        std::variant<platform::FileFailure, ProjectFailure> cause;
        bool scene_published{};
    };

    using VProjectSceneFailure = std::variant<
        EProjectSceneError,
        error::Error,
        platform::FileFailure,
        scene::ScenePackageFailure,
        scene::SceneRuntimeFailure,
        ProjectFailure,
        SceneCreationFailure,
        VSceneEditFailure>;
    template <class T> using ProjectSceneResult = cxx::expected<T, VProjectSceneFailure>;
    [[nodiscard]] std::string describeProjectSceneFailure(const VProjectSceneFailure&);
    struct SceneViewportTag;
    using SceneViewportId = cxx::SlotKey<SceneViewportTag>;

    enum class EViewportCamera : std::uint8_t
    {
        EDITOR,
        PRIMARY,
        SCENE
    };

    // Editor selection policy. RenderSystem only receives a resolved ECS CameraMan.
    struct ViewportCameraSelection final
    {
        EViewportCamera kind{EViewportCamera::EDITOR};
        world::WorldObjectId object;
        bool operator==(const ViewportCameraSelection&) const noexcept = default;
    };

    struct SceneViewportStatus final
    {
        rdesc::UiImageToken image;
        render::TargetId target;
        render::TargetDesc description;
        scene::SceneInstanceId instance;
        simulation::ecs::Entity camera{simulation::ecs::NullEntity};
        render::RenderError error;
        bool ready{}, pending{};
    };

    enum class ESceneRunMode : std::uint8_t
    {
        GRAPHICAL,
        HEADLESS
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
        [[nodiscard]] asset::AssetId activeDocument() const noexcept;
        [[nodiscard]] bool selectDocument(asset::AssetId) noexcept;
        // Main snapshot -> CPU encoding -> blocking atomic write -> Main revision acknowledgement.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> save(asset::AssetId) noexcept;
        [[nodiscard]] bool saving(asset::AssetId) const noexcept;
        // Preview builds an independent Registry from the captured author revision, never copies Render IDs.
        [[nodiscard]] ProjectSceneResult<scene::SceneInstanceId> startPlay(asset::AssetId, ESceneRunMode) noexcept;
        void stopPlay(asset::AssetId) noexcept;
        [[nodiscard]] scene::SceneInstanceId playInstance(asset::AssetId) const noexcept;
        [[nodiscard]] bool stoppingPlay(asset::AssetId) const noexcept;
        [[nodiscard]] ProjectSceneResult<void> close(asset::AssetId) noexcept;
        void closeAll() noexcept;
        // Finite Main-thread close. Continues actual viewport/Scene/producer work; no nested event pump.
        [[nodiscard]] exec::task<FrameworkResult<void>> closeAsync() noexcept;
        // Failed close keeps its actual records. Call only after RenderExecution has joined Server;
        // waits for outstanding callbacks before discarding discharged logical handles.
        [[nodiscard]] exec::task<void> finishCloseAfterServerStop() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] const std::optional<VProjectSceneFailure>& failure() const noexcept;
        // Called after the project publishes a complete VFS version, independently of viewport changes.
        void setAssetRevision(std::uint64_t) noexcept;

        [[nodiscard]] ProjectSceneResult<SceneViewportId> addViewport(
            asset::AssetId,
            std::uint32_t width,
            std::uint32_t height,
            bool play = false
        ) noexcept;
        [[nodiscard]] bool updateViewport(SceneViewportId, std::uint32_t width, std::uint32_t height) noexcept;
        // Selection is by author identity, never by an Entity from another Registry (including Play).
        [[nodiscard]] bool selectViewportCamera(SceneViewportId, ViewportCameraSelection) noexcept;
        // Writes the temporary ECS camera only, in SceneRuntime's edit safe point. No persistent pose mirror.
        [[nodiscard]] bool setViewportCameraTransform(SceneViewportId, const simulation::ecs::Transform3D&) noexcept;
        void closeViewport(SceneViewportId) noexcept;
        [[nodiscard]] SceneViewportStatus viewport(SceneViewportId) const noexcept;

        object::TSignal<asset::AssetId> opened{*this};
        object::TSignal<> changed{*this};
        object::TSignal<SceneViewportId> viewportChanged{*this};

    private:
        void event(object::EventView&) noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
