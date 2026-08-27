#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/simulation/ScriptComponent.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>
#include <lux/engine/simulation/script_binding/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace lux::simulation
{
    [[noreturn]] LUX_ENGINE_SIMULATION_SCRIPT_BINDING_PUBLIC
    void scriptBindingContractFailure() noexcept;

    enum class EScriptBindingError : std::uint8_t
    {
        ALLOCATION_FAILURE,
        CAPACITY_EXCEEDED,
        ASSET_NOT_FOUND,
        INVALID_ASSET,
        SYMBOL_NOT_FOUND,
        TARGET_SYSTEM_NOT_FOUND,
        TARGET_SYSTEM_AMBIGUOUS,
        TARGET_TYPE_MISMATCH,
        MEMBER_NOT_FOUND,
        SCOPE_MISMATCH,
        CARDINALITY_MISMATCH,
        SINGLE_HOOK_MULTIPLE_HANDLERS,
        SIGNATURE_MISMATCH,
        BACKEND_NOT_AVAILABLE,
        DUPLICATE_BACKEND_KIND,
        BACKEND_CONSTRUCTION_FAILURE,
        EXECUTABLE_CONTRACT_MISMATCH,
        UNSUPPORTED_MARSHAL_TYPE,
        INVOCATION_FAILURE,
        INVALID_SLOT,
        INVALID_ENTITY,
        SESSION_SHUT_DOWN,
    };

    struct ScriptHookSlot final
    {
        std::uint32_t value{~std::uint32_t{}};
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != ~std::uint32_t{};
        }
        friend bool operator==(ScriptHookSlot, ScriptHookSlot) noexcept = default;
    };

    struct ScriptEventSlot final
    {
        std::uint32_t value{~std::uint32_t{}};
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != ~std::uint32_t{};
        }
        friend bool operator==(ScriptEventSlot, ScriptEventSlot) noexcept = default;
    };

    struct ScriptBindingCapacities final
    {
        std::size_t mount_instances{};
        std::size_t prepared_methods{};
        std::size_t scripted_entities{};
        std::size_t dispatch_target_ranges{};
        std::size_t dispatch_handlers{};
        std::size_t dirty_entities{};
        std::size_t failures{};
    };

    struct ResolvedScriptAsset final
    {
        const lux::asset::ScriptAssetContent* asset{};
        void* lease{};
        void (*release)(void* lease) noexcept{};
    };

    struct ScriptAssetResolver final
    {
        void* context{};
        bool (*resolve)(
            void* context,
            const lux::asset::AssetId& id,
            ResolvedScriptAsset& result
        ) noexcept{};
    };

    enum class EScriptHostCommand : std::uint8_t
    {
        CREATE_ENTITY,
        DESTROY_ENTITY,
        EMPLACE_COMPONENT,
        REMOVE_COMPONENT,
    };

    struct ScriptHostApi final
    {
        void* context{};
        const void* (*read)(
            void* context,
            ecs::Entity entity,
            std::uint64_t component_type
        ) noexcept{};
        bool (*patch)(
            void* context,
            ecs::Entity entity,
            std::uint64_t component_type,
            const void* value
        ) noexcept{};
        bool (*command)(
            void* context,
            EScriptHostCommand command,
            ecs::Entity entity,
            std::uint64_t component_type,
            const void* value
        ) noexcept{};
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_BINDING_PUBLIC
        ScriptInstanceHostContext final
    {
      public:
        ScriptInstanceHostContext() noexcept = default;

        [[nodiscard]] bool attached() const noexcept;
        [[nodiscard]] ecs::Entity self() const noexcept;
        [[nodiscard]] const void* read(std::uint64_t component_type) const noexcept;
        [[nodiscard]] bool patch(
            std::uint64_t component_type,
            const void* value
        ) const noexcept;
        [[nodiscard]] bool command(
            EScriptHostCommand command,
            std::uint64_t component_type = 0U,
            const void* value = nullptr
        ) const noexcept;

      private:
        void attach(const ScriptHostApi& api, ecs::Entity entity) noexcept;
        const ScriptHostApi* api_{};
        ecs::Entity self_{ecs::NullEntity};
        friend class ScriptBindingSession;
    };

    struct ScriptBackendInstance final
    {
        void* value{};
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value != nullptr;
        }
    };

    struct ScriptInstanceCreateContext final
    {
        lux::asset::AssetId script;
        ScriptMountId mount;
        ecs::Entity self{ecs::NullEntity};
        ScriptInstanceHostContext* host{};
    };

    enum class EScriptBackendResult : std::uint8_t
    {
        SUCCESS,
        UNSUPPORTED_MODEL,
        UNSUPPORTED_SIGNATURE,
        UNSUPPORTED_MARSHAL_TYPE,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
        CONSTRUCTION_FAILURE,
        EXECUTABLE_CONTRACT_MISMATCH,
    };

    struct ScriptBackendDescriptor final
    {
        lux::rdesc::Script::Kind kind{lux::rdesc::Script::Kind::UNKNOWN};
        void* context{};
        EScriptBackendResult (*createInstance)(
            void* context,
            const ScriptInstanceCreateContext& create_context,
            const lux::asset::ScriptAssetContent& asset,
            ScriptBackendInstance& result
        ) noexcept{};
        EScriptBackendResult (*prepareMethod)(
            void* context,
            ScriptBackendInstance instance,
            const lux::rdesc::ScriptFunction& function,
            lux::script::BoundScriptCall& result
        ) noexcept{};
        void (*releaseMethod)(
            void* context,
            ScriptBackendInstance instance,
            lux::script::BoundScriptCall call
        ) noexcept{};
        void (*destroyInstance)(
            void* context,
            ScriptBackendInstance instance
        ) noexcept{};
    };

    struct ScriptBindingFailure final
    {
        EScriptBindingError error{EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE};
        ScriptMountId mount;
        lux::script::ScriptSymbolId symbol{};
        ecs::Entity entity{ecs::NullEntity};
        std::int32_t status{};
    };

    struct ScriptDispatchResult final
    {
        std::int32_t status{};
        std::size_t calls{};
        std::size_t failures{};
    };

    struct ScriptBindingInstrumentation final
    {
        std::size_t asset_resolutions{};
        std::size_t target_resolutions{};
        std::size_t entities_examined{};
        std::size_t target_range_lookups{};
        std::size_t handlers_visited{};
        std::size_t target_ranges_built{};
        std::size_t dispatch_handlers_built{};
        std::size_t instance_creates{};
        std::size_t method_prepares{};
        std::size_t frame_builds{};
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_BINDING_PUBLIC ScriptBindingSession final
    {
      public:
        struct State;

        [[nodiscard]] static lux::cxx::expected<
            ScriptBindingSession,
            EScriptBindingError> create(
                SimulationDescription description,
                ecs::Registry& registry,
                ScriptBindingCapacities capacities,
                ScriptAssetResolver resolver,
                std::span<const ScriptBackendDescriptor> backends,
                ScriptHostApi host_api = {}
            ) noexcept;

        ScriptBindingSession(ScriptBindingSession&&) noexcept;
        ScriptBindingSession& operator=(ScriptBindingSession&&) noexcept;
        ~ScriptBindingSession();

        ScriptBindingSession(const ScriptBindingSession&) = delete;
        ScriptBindingSession& operator=(const ScriptBindingSession&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        prepare() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        applyQuiescentMutations() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError>
        shutdown() noexcept;

        [[nodiscard]] ScriptHookSlot hookSlot(
            std::string_view system_instance,
            std::string_view hook
        ) const noexcept;
        [[nodiscard]] ScriptEventSlot eventSlot(
            std::string_view system_instance,
            std::string_view event
        ) const noexcept;

        [[nodiscard]] ScriptDispatchResult dispatchHook(
            ScriptHookSlot hook,
            const lux_script_call_frame& frame
        ) noexcept;
        [[nodiscard]] ScriptDispatchResult dispatchEvent(
            ScriptEventSlot event,
            ecs::Entity target,
            const lux_script_call_frame& live_frame
        ) noexcept;

        void clearFailures() noexcept;
        [[nodiscard]] std::span<const ScriptBindingFailure> failures() const
            noexcept;
        [[nodiscard]] std::size_t instanceCount() const noexcept;
        [[nodiscard]] std::size_t preparedMethodCount() const noexcept;
        [[nodiscard]] const ScriptBindingInstrumentation& instrumentation() const
            noexcept;

      private:
        explicit ScriptBindingSession(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
