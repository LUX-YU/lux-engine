#pragma once
#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

namespace lux::simulation::script
{
    namespace detail { class ScriptTimers; }

    // Minted by the actual built-in owner. A method name or a public publication cannot grant this route.
    class PreparedLocalAsyncStart final
    {
    public:
        PreparedLocalAsyncStart() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept { return start_ != nullptr; }
        [[nodiscard]] std::uint8_t argumentCount() const noexcept { return argument_count_; }
        [[nodiscard]] ScriptStepResult start(ScriptStepContext& step,
            std::span<const lux::script::ScriptAbilityInputSlot> arguments) const noexcept
        {
            return start_ ? start_(owner_, step, arguments) : ScriptStepResult::failed(-1);
        }
        template<class... Arguments>
        [[nodiscard]] ScriptStepResult startTyped(ScriptStepContext& step, Arguments&... arguments) const noexcept
        {
            const std::array<lux::script::ScriptAbilityInputSlot, sizeof...(Arguments)> inputs{{
                {lux::semantic::TypeTraits<std::remove_cvref_t<Arguments>>::AbiKind, {}, sizeof(Arguments),
                    lux::semantic::typeId(lux::semantic::TypeTraits<std::remove_cvref_t<Arguments>>::CanonicalName),
                    const_cast<std::remove_cvref_t<Arguments>*>(std::addressof(arguments))}...
            }};
            return start(step, inputs);
        }
    private:
        friend class detail::ScriptTimers;
        using Start = ScriptStepResult (*)(void*, ScriptStepContext&,
            std::span<const lux::script::ScriptAbilityInputSlot>) noexcept;
        PreparedLocalAsyncStart(void* owner, Start start, std::uint8_t arguments) noexcept
            : owner_(owner), start_(start), argument_count_(arguments) {}
        void* owner_{};
        Start start_{};
        std::uint8_t argument_count_{};
    };

    class PreparedLocalAsyncCatalog final
    {
    public:
        PreparedLocalAsyncCatalog() noexcept = default;
        [[nodiscard]] PreparedLocalAsyncStart resolve(lux::script::ScriptApiMethodIdView method,
            void* context, const void* dispatch) const noexcept
        {
            return resolve_ && context == context_ && dispatch == dispatch_ ? resolve_(context_, method) :
                PreparedLocalAsyncStart{};
        }
    private:
        friend class detail::ScriptTimers;
        using Resolve = PreparedLocalAsyncStart (*)(void*, lux::script::ScriptApiMethodIdView) noexcept;
        PreparedLocalAsyncCatalog(void* context, const void* dispatch, Resolve resolve) noexcept
            : context_(context), dispatch_(dispatch), resolve_(resolve) {}
        void* context_{};
        const void* dispatch_{};
        Resolve resolve_{};
    };
}
