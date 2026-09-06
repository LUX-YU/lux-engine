#pragma once

#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/simulation/ScriptBinding.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/scene/script_description/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lux::scene::script
{
    using lux::simulation::SimulationDescription;
    using lux::simulation::script::ScriptMountId;
    using lux::simulation::script::HookScriptTarget;
    using lux::simulation::script::EventScriptTarget;
    using lux::simulation::script::ScriptBindingTarget;
    using lux::simulation::script::ScriptBindingDescription;

    struct SimulationScriptMount final
    {
        friend constexpr bool operator==(SimulationScriptMount, SimulationScriptMount) noexcept = default;
    };

    struct EntityScriptMount final
    {
        lux::world::WorldObjectId object;

        friend bool operator==(const EntityScriptMount&, const EntityScriptMount&) noexcept = default;
    };

    using ScriptMountScope = std::variant<
        SimulationScriptMount,
        EntityScriptMount
    >;

    struct ScriptMountDescription final
    {
        ScriptMountId               id;
        lux::asset::AssetId         asset;
        ScriptMountScope            scope;
        bool                        enabled{true};
        std::vector<ScriptBindingDescription> bindings;

        friend bool operator==(const ScriptMountDescription&, const ScriptMountDescription&) noexcept = default;
    };

    class LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC ScriptSystemDescription final
    {
    public:
        static constexpr std::uint32_t kSchemaVersion{1U};

        [[nodiscard]] std::span<const ScriptMountDescription> mounts() const noexcept
        {
            return mounts_;
        }

      private:
        std::vector<ScriptMountDescription> mounts_;

        friend class ScriptSystemDescriptionBuilder;
    };

    enum class EScriptSystemDescriptionError : std::uint8_t
    {
        INVALID_MOUNT,
        DUPLICATE_MOUNT_ID,
        DUPLICATE_BINDING,
        TARGET_NOT_FOUND,
        SCOPE_MISMATCH,
        INPUT_BUDGET_EXCEEDED,
        OUTPUT_BUDGET_EXCEEDED,
        DECODED_BUDGET_EXCEEDED,
        CORRUPT_WIRE,
        ALLOCATION_FAILURE,
    };

    class LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC ScriptSystemDescriptionBuilder final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, EScriptSystemDescriptionError>
        addMount(ScriptMountDescription mount) noexcept;

        [[nodiscard]] lux::cxx::expected<ScriptSystemDescription, EScriptSystemDescriptionError>
        build(const SimulationDescription& simulation) && noexcept;

      private:
        struct BindingHash final
        {
            [[nodiscard]] std::size_t operator()(const ScriptBindingDescription& binding) const noexcept;
        };

        std::vector<ScriptMountDescription> mounts_;
        std::unordered_set<std::uint64_t> mount_ids_;
        std::unordered_set<lux::world::WorldObjectId, lux::world::WorldObjectIdHash> entity_objects_;
    };
}
