#pragma once

#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/script/visibility.h>
#include <lux/engine/world/WorldObjectId.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace lux::simulation::script
{
    struct ScriptMountId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr auto operator<=>(ScriptMountId, ScriptMountId)
            noexcept = default;
    };

    struct SimulationScriptMount final
    {
        friend constexpr bool operator==(
            SimulationScriptMount,
            SimulationScriptMount
        ) noexcept = default;
    };

    struct EntityScriptMount final
    {
        lux::world::WorldObjectId object;

        friend bool operator==(
            const EntityScriptMount&,
            const EntityScriptMount&
        ) noexcept = default;
    };

    using ScriptMountScope = std::variant<
        SimulationScriptMount,
        EntityScriptMount>;

    struct HookScriptTarget final
    {
        SystemInstanceId system;
        HookPointId hook;

        friend constexpr bool operator==(
            HookScriptTarget,
            HookScriptTarget
        ) noexcept = default;
    };

    struct EventScriptTarget final
    {
        SystemInstanceId system;
        EventPointId event;

        friend constexpr bool operator==(
            EventScriptTarget,
            EventScriptTarget
        ) noexcept = default;
    };

    using ScriptBindingTarget = std::variant<
        HookScriptTarget,
        EventScriptTarget>;

    struct ScriptBindingDescription final
    {
        lux::script::ScriptSymbolId symbol{
            lux::script::InvalidScriptSymbolId};
        ScriptBindingTarget target;

        friend bool operator==(
            const ScriptBindingDescription&,
            const ScriptBindingDescription&
        ) noexcept = default;
    };

    struct ScriptMountDescription final
    {
        ScriptMountId id;
        lux::asset::AssetId asset;
        ScriptMountScope scope;
        bool enabled{true};
        std::vector<ScriptBindingDescription> bindings;

        friend bool operator==(
            const ScriptMountDescription&,
            const ScriptMountDescription&
        ) noexcept = default;
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptSystemDescription final
    {
      public:
        static constexpr std::uint32_t kSchemaVersion{1U};

        [[nodiscard]] std::span<const ScriptMountDescription> mounts() const
            noexcept
        {
            return mounts_;
        }

        [[nodiscard]] const ScriptMountDescription* findMount(
            ScriptMountId id
        ) const noexcept;

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

    class LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
        ScriptSystemDescriptionBuilder final
    {
      public:
        [[nodiscard]] lux::cxx::expected<
            void,
            EScriptSystemDescriptionError>
        addMount(ScriptMountDescription mount) noexcept;

        [[nodiscard]] lux::cxx::expected<
            ScriptSystemDescription,
            EScriptSystemDescriptionError>
        build(const SimulationDescription& simulation) && noexcept;

      private:
        std::vector<ScriptMountDescription> mounts_;
    };
}
