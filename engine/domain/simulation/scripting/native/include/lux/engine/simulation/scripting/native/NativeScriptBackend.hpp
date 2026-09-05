#pragma once

#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/function/script/native/ScriptAbilityNative.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>
#include <lux/engine/simulation/scripting/native/visibility.h>

#include <cstddef>
#include <memory>
#include <span>

namespace lux::simulation::script
{
    namespace detail
    {
        struct NativeAbilityProjectionAccess;
    }

    struct NativeScriptRecordLayoutResolver final
    {
        void* context{};
        bool (*resolve)(
            void* context,
            std::uint64_t type_id,
            std::string_view canonical_name,
            lux_script_type_desc& result
        ) noexcept{};
    };

    struct ResolvedNativeModule final
    {
        const lux::script::NativeModule* module{};
        void* lease{};
        void (*release)(void* lease) noexcept{};
    };

    struct NativeModuleResolver final
    {
        void* context{};
        bool (*resolve)(
            void* context,
            const lux::asset::AssetId& asset,
            const lux::script::ScriptArtifact& artifact,
            ResolvedNativeModule& result
        ) noexcept{};
    };

    struct NativeScriptBackendConfig final
    {
        std::size_t module_capacity{};
        std::size_t instance_capacity{};
        std::size_t prepared_call_capacity{};
        std::size_t continuation_capacity{};
        std::size_t max_ability_imports_per_module{};
        std::size_t max_continuation_frame_bytes{};
        std::size_t continuation_frame_storage_bytes{};
        std::size_t continuation_frame_storage_alignment{alignof(std::max_align_t)};
        NativeScriptRecordLayoutResolver record_layouts;
        std::size_t max_event_wait_imports_per_module{64U};
        std::span<const lux::script::native::ScriptAbilityNativeContribution> abilities;
        std::span<const detail::StorageClassPlan> state_storage_classes;
        std::size_t state_storage_bytes{};
        std::span<const detail::StorageClassPlan> continuation_frame_classes;
    };

    struct NativeScriptBackendStats final
    {
        std::size_t frame_storage_bytes{};
        std::size_t active_frames{};
        std::size_t frame_high_water{};
        std::size_t frame_capacity_failures{};
        std::size_t heap_frame_allocations{};
        std::size_t state_storage_bytes{};
        std::size_t state_metadata_bytes{};
        std::size_t active_states{};
        std::size_t state_high_water{};
        std::uint64_t state_acquire_steps{};
        std::uint64_t state_release_steps{};
        std::size_t frame_metadata_bytes{};
        std::uint64_t frame_acquire_steps{};
        std::uint64_t frame_release_steps{};
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_NATIVE_PUBLIC NativeScriptBackend final
    {
      public:
        NativeScriptBackend(
            NativeModuleResolver resolver,
            NativeScriptBackendConfig config
        ) noexcept;
        ~NativeScriptBackend();

        NativeScriptBackend(NativeScriptBackend&&) noexcept;
        NativeScriptBackend& operator=(
            NativeScriptBackend&&
        ) noexcept;
        NativeScriptBackend(const NativeScriptBackend&) = delete;
        NativeScriptBackend& operator=(
            const NativeScriptBackend&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] NativeScriptBackendStats stats() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;

      private:
        friend struct detail::NativeAbilityProjectionAccess;
        struct State;
        std::unique_ptr<State> state_;
    };
}
