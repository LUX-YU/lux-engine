#pragma once

#include <lux/engine/editor/SceneEdit.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <map>

namespace lux::editor
{
    // The active ECS is the author truth. This document owns revision/history and immutable save inputs.
    // EditorContext owns the instance lease; it must retire that instance before destroying this document.
    class SceneDocument final : public object::LuxObject
    {
    public:
        SceneDocument(
            scene::SceneRuntime&,
            scene::SceneInstanceId,
            simulation::ecs::ComponentSchemaSet,
            scene::ScenePackage
        );

        [[nodiscard]] asset::AssetId id() const noexcept;
        [[nodiscard]] scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] bool dirty() const noexcept;
        [[nodiscard]] bool readOnly() const noexcept;
        void setReadOnly(bool) noexcept;

        [[nodiscard]] SceneEditResult<void> apply(const SceneEdit&) noexcept;
        [[nodiscard]] SceneEditResult<void> undo() noexcept;
        [[nodiscard]] SceneEditResult<void> redo() noexcept;
        [[nodiscard]] SceneEditResult<SceneObjectData> object(world::WorldObjectId) const noexcept;
        [[nodiscard]] SceneEditResult<std::vector<world::WorldObjectId>> objects() const noexcept;

        // Main captures author values; workers only encode/write this immutable package.
        [[nodiscard]] SceneEditResult<scene::ScenePackage> capture() const noexcept;
        // A successful file publication advances only its captured revision, never a newer edit.
        [[nodiscard]] SceneEditResult<void> saved(scene::ScenePackage, std::uint64_t captured_revision) noexcept;
        [[nodiscard]] const scene::ScenePackage& baseline() const noexcept;

        // Immutable author codecs retained by this document; tools prepare owned drafts through them.
        [[nodiscard]] const simulation::ecs::ComponentSchemaSet& schemas() const noexcept
        {
            return schemas_;
        }

        object::TSignal<> changed{*this};

    private:
        struct History final
        {
            VSceneEdit forward, inverse;
        };

        [[nodiscard]] SceneEditResult<VSceneEdit> execute(const VSceneEdit&) noexcept;
        void changedRevision() noexcept;

        scene::SceneRuntime& runtime_;
        scene::SceneInstanceId instance_;
        simulation::ecs::ComponentSchemaSet schemas_;
        scene::ScenePackage baseline_;
        std::vector<History> history_;
        std::map<std::uint32_t, scene::PartitionRetention> retained_partitions_;
        std::size_t history_position_{};
        std::uint64_t revision_{}, saved_revision_{};
        bool read_only_{};
    };
} // namespace lux::editor
