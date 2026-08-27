#pragma once

#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/simulation/SystemTypeId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace lux::simulation
{
    struct ScriptMountId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr bool operator==(
            const ScriptMountId&,
            const ScriptMountId&
        ) noexcept = default;
    };

    struct SystemHookBindingTarget final
    {
        SystemTypeId system_type;
        std::string system_instance;
        std::string hook;

        friend bool operator==(
            const SystemHookBindingTarget&,
            const SystemHookBindingTarget&
        ) noexcept = default;
    };

    struct SystemEventBindingTarget final
    {
        SystemTypeId system_type;
        std::string system_instance;
        std::string event;

        friend bool operator==(
            const SystemEventBindingTarget&,
            const SystemEventBindingTarget&
        ) noexcept = default;
    };

    enum class EBehaviorLifecyclePoint : std::uint8_t
    {
        CONSTRUCT,
        START,
        STOP,
    };

    enum class EBehaviorStopReason : std::uint32_t
    {
        MOUNT_REMOVED,
        ENTITY_DESTROYED,
        SIMULATION_STOPPED,
    };

    inline constexpr std::string_view BehaviorStopReasonCanonicalName{
        "lux.simulation.BehaviorStopReason"};

    struct BehaviorLifecycleBindingTarget final
    {
        EBehaviorLifecyclePoint point{EBehaviorLifecyclePoint::CONSTRUCT};

        friend bool operator==(
            const BehaviorLifecycleBindingTarget&,
            const BehaviorLifecycleBindingTarget&
        ) noexcept = default;
    };

    using ScriptBindingTarget = std::variant<
        SystemHookBindingTarget,
        SystemEventBindingTarget,
        BehaviorLifecycleBindingTarget>;

    struct ScriptBindingDescription final
    {
        lux::script::ScriptSymbolId function{
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
        lux::asset::AssetId script;
        std::vector<ScriptBindingDescription> bindings;

        friend bool operator==(
            const ScriptMountDescription&,
            const ScriptMountDescription&
        ) noexcept = default;
    };

    [[nodiscard]] inline bool validScriptBindingDescription(
        const ScriptBindingDescription& binding
    ) noexcept
    {
        if (binding.function == lux::script::InvalidScriptSymbolId)
            return false;
        return std::visit(
            [](const auto& target) noexcept
            {
                using Target = std::remove_cvref_t<decltype(target)>;
                if constexpr (std::is_same_v<Target, SystemHookBindingTarget>)
                    return target.system_type.valid() && !target.hook.empty();
                else if constexpr (
                    std::is_same_v<Target, SystemEventBindingTarget>)
                    return target.system_type.valid() && !target.event.empty();
                else
                    return target.point == EBehaviorLifecyclePoint::CONSTRUCT ||
                        target.point == EBehaviorLifecyclePoint::START ||
                        target.point == EBehaviorLifecyclePoint::STOP;
            },
            binding.target
        );
    }

    [[nodiscard]] inline bool validScriptMountDescription(
        const ScriptMountDescription& mount
    ) noexcept
    {
        if (!mount.id.valid() || mount.script.isNull())
            return false;
        for (std::size_t index{}; index < mount.bindings.size(); ++index)
        {
            if (!validScriptBindingDescription(mount.bindings[index]))
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (mount.bindings[previous] == mount.bindings[index])
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool validScriptMountList(
        const std::vector<ScriptMountDescription>& mounts
    ) noexcept
    {
        for (std::size_t index{}; index < mounts.size(); ++index)
        {
            if (!validScriptMountDescription(mounts[index]))
                return false;
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (mounts[previous].id == mounts[index].id)
                    return false;
            }
        }
        return true;
    }
}
