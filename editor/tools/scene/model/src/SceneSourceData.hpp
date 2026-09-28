#pragma once

#include <lux/engine/editor/scene/SceneSnapshot.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/scene/WorldMaterializer.hpp>

namespace lux::editor::scene
{
    struct SceneSource::Data final
    {
        // Code owners precede values: reverse destruction releases plugin objects before code.
        simulation::ecs::ComponentSchemaSet schemas;
        SceneConfiguration configuration;
        std::vector<lux::cxx::SharedBytes<>> volumes;
        lux::asset::PakDecodedImage package;
        simulation::ecs::Registry registry;
        simulation::ecs::WorldEntityMap identities;
        // Only unrecognized components are retained here. Recognized values live solely in registry.
        std::vector<SceneObjectData> objects;
    };

    namespace detail
    {
        class SceneBudget final
        {
        public:
            explicit SceneBudget(std::size_t limit) noexcept : remaining_(limit), limit_(limit) {}
            [[nodiscard]] bool take(std::size_t bytes) noexcept
            {
                if (bytes > remaining_)
                    return false;
                remaining_ -= bytes;
                return true;
            }
            [[nodiscard]] std::size_t remaining() const noexcept
            {
                return remaining_;
            }
            [[nodiscard]] std::size_t used() const noexcept
            {
                return limit_ - remaining_;
            }

        private:
            std::size_t remaining_;
            std::size_t limit_;
        };

        struct SceneSourceAccess final
        {
            using Data = SceneSource::Data;
            static Data& data(SceneSource& source) noexcept
            {
                return *source.data_;
            }
            static const Data& data(const SceneSource& source) noexcept
            {
                return *source.data_;
            }
            static void swap(SceneSource& a, SceneSource& b) noexcept
            {
                a.data_.swap(b.data_);
            }
            [[nodiscard]] static SceneEditResult<SceneSource> build(
                SceneConfiguration configuration,
                simulation::ecs::ComponentSchemaSet schemas,
                std::span<const SceneObjectData> objects,
                SceneBudget& budget
            );
            [[nodiscard]] static SceneEditResult<SceneConfiguration> freezeConfiguration(
                const SceneConfiguration& value,
                SceneBudget& budget
            );
            [[nodiscard]] static SceneEditResult<SceneComponentData> component(
                const Data& source,
                world::WorldObjectId object,
                const simulation::ecs::ComponentSchemaId& schema,
                SceneBudget& budget
            );
            [[nodiscard]] static SceneEditResult<std::vector<SceneObjectData>> objects(
                const Data& source,
                SceneBudget& budget
            );
            [[nodiscard]] static SceneEditResult<SceneSnapshot> capture(
                const SceneSource& source,
                sessions::ContentStamp stamp,
                SceneChangeCursor cursor,
                SnapshotBudget budget
            );
            [[nodiscard]] static SceneEditResult<void> validate(const Data& source);
            [[nodiscard]] static SceneEditResult<void> copyOpaque(const Data& from, Data& to, SceneBudget& budget);
        };

        [[nodiscard]] inline auto rejected(ESceneEditError code, world::WorldObjectId object = {}, std::size_t at = {})
        {
            return lux::cxx::unexpected(SceneEditError{code, object, at});
        }
        [[nodiscard]] std::size_t objectBytes(const SceneObjectData& object) noexcept;
    }
}
