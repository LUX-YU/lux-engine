#pragma once

#include <lux/engine/editor/scene/SceneEditError.hpp>
#include <lux/engine/editor/sessions/ContentStamp.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

namespace lux::editor::scene
{
    enum class EObjectSpace : std::uint8_t { NONE, SPACE_2D, SPACE_3D };
    struct SnapshotBudget final
    {
        std::size_t max_bytes{64 * 1024 * 1024};
    };

    struct SceneComponentData final
    {
        simulation::ecs::ComponentSchemaId schema;
        std::uint32_t version{};
        std::vector<std::byte> bytes;
        friend bool operator==(const SceneComponentData&, const SceneComponentData&) noexcept = default;
    };

    struct SceneObjectData final
    {
        world::WorldObjectId id;
        partition::PartitionOrdinal partition;
        std::vector<SceneComponentData> components;
        friend bool operator==(const SceneObjectData&, const SceneObjectData&) noexcept = default;
    };

    // The three assets are cloned by their codecs on entry. No caller-owned mutable aliases survive.
    struct SceneConfiguration final
    {
        std::shared_ptr<const lux::scene::SceneAsset> scene;
        std::shared_ptr<const world::WorldAsset> world;
        std::shared_ptr<const simulation::SimulationAsset> simulation;
    };

    namespace detail
    {
        struct SceneSourceAccess;
    }
    class SceneSession;
    class SceneReadView;

    // Author-owned CPU data. No execution systems, observers, render state or writable Registry API.
    class SceneSource final
    {
    public:
        [[nodiscard]] static SceneEditResult<SceneSource> create(
            const lux::scene::ScenePackage& package,
            simulation::ecs::ComponentSchemaSet schemas,
            SnapshotBudget budget = {}
        );
        ~SceneSource() noexcept;
        SceneSource(SceneSource&&) noexcept;
        SceneSource& operator=(SceneSource&&) noexcept;
        SceneSource(const SceneSource&) = delete;
        SceneSource& operator=(const SceneSource&) = delete;

    private:
        friend class SceneSession;
        friend class SceneReadView;
        friend struct detail::SceneSourceAccess;
        struct Data;
        explicit SceneSource(std::unique_ptr<Data> data) noexcept;
        std::unique_ptr<Data> data_;
    };
}
