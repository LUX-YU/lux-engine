#pragma once

#include <lux/cxx/compile_time/type_info.hpp>
#include <lux/engine/render/core/ConfigValue.hpp>
#include <memory>

namespace lux::render
{
    class RenderServer;

    // One independently owned state is constructed for each selected Effect contribution.
    class EffectStateFactory final
    {
    public:
        template <class State, RenderResult<std::unique_ptr<State>> (*Create)() noexcept>
        [[nodiscard]] static EffectStateFactory declare() noexcept
        {
            static_assert(std::is_nothrow_destructible_v<State>);
            EffectStateFactory result;
            result.type_ = cxx::make_basic_type_info<State>();
            result.create_ = []() noexcept -> RenderResult<void*>
            {
                auto state = Create();
                if (!state) return cxx::unexpected(state.error());
                if (!*state) return cxx::unexpected(RenderError{kInvalidConfig});
                return state->release();
            };
            result.destroy_ = [](void* state) noexcept { delete static_cast<State*>(state); };
            return result;
        }

    private:
        friend class RenderServer;
        cxx::basic_type_info type_;
        RenderResult<void*> (*create_)() noexcept = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
    };
} // namespace lux::render
