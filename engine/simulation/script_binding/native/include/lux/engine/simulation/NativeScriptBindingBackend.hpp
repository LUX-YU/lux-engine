#pragma once

#include <lux/engine/function/script/native/NativeModule.hpp>
#include <lux/engine/simulation/ScriptBindingSession.hpp>
#include <lux/engine/simulation/script_binding/native/visibility.h>

#include <memory>

namespace lux::simulation
{
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

    class LUX_ENGINE_SIMULATION_SCRIPT_BINDING_NATIVE_PUBLIC
        NativeScriptBindingBackend final
    {
      public:
        explicit NativeScriptBindingBackend(
            std::shared_ptr<lux::script::NativeModule> module,
            std::size_t instance_capacity,
            NativeScriptRecordLayoutResolver record_layouts = {}
        ) noexcept;
        ~NativeScriptBindingBackend();

        NativeScriptBindingBackend(NativeScriptBindingBackend&&) noexcept;
        NativeScriptBindingBackend& operator=(
            NativeScriptBindingBackend&&
        ) noexcept;
        NativeScriptBindingBackend(const NativeScriptBindingBackend&) = delete;
        NativeScriptBindingBackend& operator=(
            const NativeScriptBindingBackend&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
