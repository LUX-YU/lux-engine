#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/resource/asset/script/ScriptAsset.hpp>
#include <lux/engine/simulation/ScriptMountFacts.hpp>
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
    enum class EScriptBindingError : std::uint8_t
    {
        ALLOCATION_FAILURE,
        CAPACITY_EXCEEDED,
        ASSET_NOT_FOUND,
        INVALID_ASSET,
        SYMBOL_NOT_FOUND,
        SYSTEM_NOT_FOUND,
        MEMBER_NOT_FOUND,
        SCOPE_MISMATCH,
        SIGNATURE_MISMATCH,
        BACKEND_NOT_FOUND,
        BACKEND_CONSTRUCTION_FAILURE,
        INVOCATION_FAILURE,
        INVALID_PRODUCER,
        INVALID_SLOT,
        INVALID_ENTITY,
        OCCURRENCE_CAPACITY_EXCEEDED,
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
        std::size_t prepared_calls{};
        std::size_t entity_slots{};
        std::size_t producer_count{};
        std::size_t occurrences_per_producer{};
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

    struct ScriptPrepareContext final
    {
        lux::asset::AssetId script;
        ecs::Entity entity{ecs::NullEntity};
        std::uint32_t mount_ordinal{};
        std::uint32_t binding_ordinal{};
    };

    enum class EScriptBackendPrepareResult : std::uint8_t
    {
        SUCCESS,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
        SIGNATURE_MISMATCH,
        CONSTRUCTION_FAILURE,
    };

    struct ScriptBackendDescriptor final
    {
        lux::rdesc::Script::Kind kind{lux::rdesc::Script::Kind::UNKNOWN};
        void* context{};
        EScriptBackendPrepareResult (*prepare)(
            void* context,
            const ScriptPrepareContext& instance,
            const lux::asset::ScriptAssetContent& asset,
            const lux::rdesc::ScriptFunction& function,
            lux::script::BoundScriptCall& result
        ) noexcept{};
        void (*release)(
            void* context,
            lux::script::BoundScriptCall call
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

    struct ScriptHostContext final
    {
        const ScriptHostApi* api{};
        ecs::Entity self{ecs::NullEntity};
    };

    struct ScriptBindingFailure final
    {
        EScriptBindingError error{EScriptBindingError::BACKEND_CONSTRUCTION_FAILURE};
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

    class ScriptBindingSession;

    class LUX_ENGINE_SIMULATION_SCRIPT_BINDING_PUBLIC ScriptEventWriter final
    {
      public:
        ScriptEventWriter() noexcept = default;
        ScriptEventWriter(ScriptEventWriter&&) noexcept = default;
        ScriptEventWriter& operator=(ScriptEventWriter&&) noexcept = default;
        ScriptEventWriter(const ScriptEventWriter&) = delete;
        ScriptEventWriter& operator=(const ScriptEventWriter&) = delete;

        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> emit(
            ScriptEventSlot event,
            const lux_script_call_frame& frame
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> emit(
            ScriptEventSlot event,
            ecs::Entity target,
            const lux_script_call_frame& frame
        ) noexcept;

      private:
        ScriptEventWriter(
            ScriptBindingSession& session,
            std::size_t producer
        ) noexcept;

        ScriptBindingSession* session_{};
        std::size_t producer_{};
        friend class ScriptBindingSession;
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
        void beginUpdate() noexcept;

        [[nodiscard]] ScriptHookSlot hookSlot(
            std::string_view system_instance,
            std::string_view hook
        ) const noexcept;
        [[nodiscard]] ScriptEventSlot eventSlot(
            std::string_view system_instance,
            std::string_view event
        ) const noexcept;
        [[nodiscard]] ScriptEventWriter writer(std::size_t producer) noexcept;
        [[nodiscard]] ScriptDispatchResult dispatchHook(
            ScriptHookSlot hook,
            const lux_script_call_frame& frame
        ) noexcept;

        [[nodiscard]] std::span<const ScriptBindingFailure> failures() const
            noexcept;
        [[nodiscard]] std::size_t preparedCallCount() const noexcept;
        [[nodiscard]] std::size_t pendingOccurrenceCount() const noexcept;
        [[nodiscard]] std::size_t hotPathNameLookupCount() const noexcept;
        [[nodiscard]] std::size_t hotPathAssetLookupCount() const noexcept;
        [[nodiscard]] std::size_t hotPathSceneScanCount() const noexcept;

      private:
        explicit ScriptBindingSession(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] lux::cxx::expected<void, EScriptBindingError> emit(
            std::size_t producer,
            ScriptEventSlot event,
            ecs::Entity target,
            const lux_script_call_frame& frame
        ) noexcept;

        std::unique_ptr<State> state_;
        friend class ScriptEventWriter;
    };
}
