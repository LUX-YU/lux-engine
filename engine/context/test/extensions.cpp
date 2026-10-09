#include <lux/engine/EngineContext.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>

#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

namespace
{
    void require(bool value, const char* message) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::abort();
        }
    }

    struct CountingSurface final
    {
        unsigned& destroyed;

        ~CountingSurface() noexcept
        {
            ++destroyed;
        }
    };

    template <class T>
    concept HasRuntimeBind = requires(T& table, CountingSurface& value) { table.bind(value); };
} // namespace

int main()
{
    using namespace lux;
    using engine::ContextExtensions;
    static_assert(!HasRuntimeBind<ContextExtensions>);
    static_assert(!std::is_move_assignable_v<ContextExtensions>);
    static_assert(!std::is_copy_constructible_v<ContextExtensions>);
    static_assert(std::is_same_v<
                  decltype(std::declval<const ContextExtensions&>().find<CountingSurface>()),
                  const CountingSurface*>);

    unsigned destroyed{};
    {
        CountingSurface surface{destroyed};
        simulation::SimulationSystemRegistry systems;
        ContextExtensions::Composition composition;
        require(composition.bind(systems).has_value(), "actual domain registry binding");
        require(composition.bind(surface).has_value(), "second independent surface");
        const auto duplicate = composition.bind(surface);
        require(
            !duplicate && duplicate.error() == engine::EContextExtensionError::DUPLICATE_TYPE,
            "duplicate does not replace original binding"
        );
        auto context = engine::EngineContext::create({1, 16, 16, {8}}, {0, 16}, std::move(composition));
        require(context.has_value(), "real headless EngineContext");
        for (unsigned index{}; index < 10000; ++index)
        {
            require((*context)->extensions().find<CountingSurface>() == &surface, "stable borrowed identity");
            require((*context)->extensions().find<simulation::SimulationSystemRegistry>() == &systems, "domain owner");
            require((*context)->extensions().find<int>() == nullptr, "missing is not lazy construction");
        }
        const auto& immutable_context = **context;
        require(std::as_const(immutable_context).extensions().find<CountingSurface>() == &surface, "const lookup");
        context->reset();
        require(destroyed == 0, "Context cannot delete a borrowed surface");
    }
    require(destroyed == 1, "original owner releases once after Context");
    auto empty = engine::EngineContext::create({1, 16, 16, {8}}, {0, 16});
    require(empty.has_value() && !(*empty)->extensions().find<CountingSurface>(), "independent empty Context");
    std::puts("PASS: composition-only bindings, actual domain lookup, const access, no service construction/ownership");
}
