#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/core/execution/TaskInfo.hpp>
#include <lux/engine/editor/FrameworkResult.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/scene/ScenePackageFile.hpp>

#include <filesystem>
#include <memory>

namespace lux::editor
{
    class EditorContext;
    enum class EProjectSceneError : std::uint8_t
    {
        INVALID_DOCUMENT,
        DUPLICATE_DOCUMENT,
        NOT_OPEN,
        CLOSED,
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
    enum class ESceneRunMode : std::uint8_t
    {
        GRAPHICAL,
        HEADLESS
    };

    class ProjectScenes;

    class SceneCreationCandidate final
    {
    public:
        ~SceneCreationCandidate() noexcept;
        SceneCreationCandidate(const SceneCreationCandidate&) = delete;
        SceneCreationCandidate& operator=(const SceneCreationCandidate&) = delete;

    private:
        friend class ProjectScenes;
        struct Impl;
        explicit SceneCreationCandidate(std::unique_ptr<Impl>) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    using SceneCreationResult = ProjectSceneResult<std::unique_ptr<SceneCreationCandidate>>;
    using SceneCreationCompletion = cxx::move_only_function<void(SceneCreationResult) noexcept>;

    // One project's author documents and preview instances. Professional tools register their
    // own Scene close participation; this collection does not own their outputs or ECS bindings.
    class ProjectScenes final : public object::LuxObject
    {
    public:
        ProjectScenes(EditorContext&, std::vector<scene::ResolvedSceneDependency> = {}) noexcept;
        ~ProjectScenes() noexcept;
        // Admission creates one existing monitored Task. IO, decode and Main adoption use the shared execution.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> open(std::filesystem::path) noexcept;
        [[nodiscard]] ProjectSceneResult<execution::TaskId>
        create(ProjectManifest::SceneRecord, SceneCreateSelection) noexcept;
        // Cold preflight: no runtime Scene/GPU objects. The completion runs on Main and owns its result.
        [[nodiscard]] ProjectSceneResult<execution::TaskId>
        prepare(ProjectManifest::SceneRecord, SceneCreateSelection, SceneCreationCompletion) noexcept;
        // Publish and adopt the exact prepared package and cached Builder qualification.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> create(std::unique_ptr<SceneCreationCandidate>) noexcept;
        [[nodiscard]] SceneDocument* find(asset::AssetId) noexcept;
        [[nodiscard]] const std::filesystem::path* path(asset::AssetId) const noexcept;
        [[nodiscard]] std::vector<asset::AssetId> documents() const;
        [[nodiscard]] asset::AssetId activeDocument() const noexcept;
        [[nodiscard]] bool selectDocument(asset::AssetId) noexcept;
        // Main snapshot -> CPU encoding -> blocking atomic write -> Main revision acknowledgement.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> save(asset::AssetId) noexcept;
        [[nodiscard]] bool saving(asset::AssetId) const noexcept;
        // Admits a finite preview task. playInstance() becomes valid only after its capability plan and Scene adopt.
        // The independent Registry is built from the author snapshot, never copied Render IDs.
        [[nodiscard]] ProjectSceneResult<execution::TaskId> startPlay(asset::AssetId, ESceneRunMode) noexcept;
        void stopPlay(asset::AssetId) noexcept;
        [[nodiscard]] scene::SceneInstanceId playInstance(asset::AssetId) const noexcept;
        [[nodiscard]] bool stoppingPlay(asset::AssetId) const noexcept;
        [[nodiscard]] ProjectSceneResult<void> close(asset::AssetId) noexcept;
        [[nodiscard]] exec::task<FrameworkResult<void>> initializeStartupScene(asset::AssetId) noexcept;
        void requestClose() noexcept;
        // Finite Main-thread close. Continues actual Scene/producer work; no nested event pump.
        [[nodiscard]] exec::task<FrameworkResult<void>> closeAsync() noexcept;
        // Failed close keeps its actual records. Call only after RenderExecution has joined Server;
        // waits for outstanding callbacks before discarding discharged logical handles.
        [[nodiscard]] exec::task<FrameworkResult<void>> recoverClose() noexcept;
        [[nodiscard]] bool settled() const noexcept;
        [[nodiscard]] const std::optional<VProjectSceneFailure>& failure() const noexcept;

        object::TSignal<asset::AssetId> opened{*this};
        object::TSignal<> changed{*this};

    private:
        void event(object::EventView&) noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor
