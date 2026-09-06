#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/simulation/ecs/Entity.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>
#include <span>

namespace lux::simulation::script
{
    struct SimulationScriptScope final
    {
        friend constexpr bool operator==(
            SimulationScriptScope,
            SimulationScriptScope
        ) noexcept = default;
    };

    struct EntityScriptScope final
    {
        ecs::Entity self{ecs::NullEntity};

        friend constexpr bool operator==(
            EntityScriptScope,
            EntityScriptScope
        ) noexcept = default;
    };

    using ScriptInstanceScope = std::variant<
        SimulationScriptScope,
        EntityScriptScope>;

    enum class EScriptHostCommand : std::uint8_t
    {
        CREATE_ENTITY,
        DESTROY_ENTITY,
        EMPLACE_COMPONENT,
        REMOVE_COMPONENT,
    };

    struct ScriptHostComponentContract final
    {
        std::uint64_t component_type{};
        std::uint64_t semantic_type{};
        std::string_view canonical_name;
        std::uint8_t abi_kind{LUX_SCRIPT_VK_VOID};
        std::size_t size{};
        std::size_t alignment{};
    };

    struct ScriptHostApi final
    {
        void* context{};
        const void* (*read)(
            void*,
            ecs::Entity,
            std::uint64_t
        ) noexcept{};
        bool (*patch)(
            void*,
            ecs::Entity,
            std::uint64_t,
            const void*
        ) noexcept{};
        bool (*command)(
            void*,
            EScriptHostCommand,
            ecs::Entity,
            std::uint64_t,
            const void*
        ) noexcept{};
        bool (*component_contract)(
            void*,
            std::uint64_t,
            ScriptHostComponentContract&
        ) noexcept{};
    };

    class ScriptBehavior final
    {
      public:
        [[nodiscard]] bool isAttached() const noexcept { return api_ != nullptr; }

        [[nodiscard]] bool hasSelf() const noexcept
        {
            return std::holds_alternative<EntityScriptScope>(scope_);
        }

        [[nodiscard]] ecs::Entity self() const noexcept
        {
            const auto* entity = std::get_if<EntityScriptScope>(&scope_);
            return entity ? entity->self : ecs::NullEntity;
        }

        [[nodiscard]] const void* read(
            std::uint64_t component_type
        ) const noexcept
        {
            return hasSelf() && api_ && api_->read
                ? api_->read(api_->context, self(), component_type)
                : nullptr;
        }

        [[nodiscard]] bool patch(
            std::uint64_t component_type,
            const void* value
        ) const noexcept
        {
            return hasSelf() && api_ && api_->patch &&
                api_->patch(api_->context, self(), component_type, value);
        }

        [[nodiscard]] bool command(
            EScriptHostCommand command,
            std::uint64_t component_type = 0U,
            const void* value = nullptr
        ) const noexcept
        {
            return api_ && api_->command && api_->command(
                api_->context,
                command,
                self(),
                component_type,
                value
            );
        }

        [[nodiscard]] bool componentContract(
            std::uint64_t component_type,
            ScriptHostComponentContract& result
        ) const noexcept
        {
            return api_ && api_->component_contract &&
                api_->component_contract(
                    api_->context,
                    component_type,
                    result
                );
        }

      private:
        ScriptInstanceScope scope_;
        const ScriptHostApi* api_{};

        void attach(
            ScriptInstanceScope scope,
            const ScriptHostApi& api
        ) noexcept
        {
            scope_ = scope;
            api_ = &api;
        }

        friend class ScriptSystem;
    };

    struct ScriptBackendInstance final
    {
        void* value{};
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != nullptr;
        }
    };

    enum class EScriptBackendResult : std::uint8_t
    {
        SUCCESS,
        UNSUPPORTED_SIGNATURE,
        UNSUPPORTED_MARSHAL_TYPE,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
        CONSTRUCTION_FAILURE,
        EXECUTABLE_CONTRACT_MISMATCH,
        HOST_COMPONENT_CONTRACT_MISMATCH,
        HOST_CONTEXT_MISMATCH,
    };

    struct ScriptInstanceCreateContext final
    {
        lux::asset::AssetId asset;
        ScriptInstanceScope scope;
        ScriptBehavior* behavior{};
        ScriptInstanceId instance;
        std::span<const PreparedScriptApiCapability> capabilities;
        std::span<const lux::script::ScriptEventSourceDescription> events;
    };

    struct BoundScriptStepCall final
    {
        void* context{};
        ScriptStepResult (*invoke)(
            void*,
            lux_script_call_frame&,
            ScriptStepContext&,
            ScriptBackendContinuation&
        ) noexcept{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return invoke != nullptr;
        }
    };

    struct ScriptBackendPreparedMethod final
    {
        void* token{};
        lux::script::BoundScriptCall synchronous;
        BoundScriptStepCall resumable;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return token != nullptr && (static_cast<bool>(synchronous) || static_cast<bool>(resumable));
        }
    };

    struct ScriptBackendDescriptor final
    {
        lux::rdesc::Script::Kind kind{lux::rdesc::Script::Kind::UNKNOWN};
        void* context{};
        EScriptBackendResult (*createInstance)(
            void*,
            const ScriptInstanceCreateContext&,
            const lux::script::ScriptArtifact&,
            ScriptBackendInstance&
        ) noexcept{};
        EScriptBackendResult (*prepareMethod)(
            void*,
            ScriptBackendInstance,
            const lux::rdesc::ScriptFunction&,
            ScriptBackendPreparedMethod&
        ) noexcept{};
        void (*releaseMethod)(
            void*,
            ScriptBackendInstance,
            ScriptBackendPreparedMethod
        ) noexcept{};
        void (*destroyInstance)(
            void*,
            ScriptBackendInstance
        ) noexcept{};
    };
}
