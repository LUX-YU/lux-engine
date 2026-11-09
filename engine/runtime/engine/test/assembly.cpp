#include <cassert>
#include <chrono>
#include <lux/engine/Engine.hpp>
#include <lux/engine/EngineSystemDeclaration.hpp>
#include <lux/engine/core/execution/ExecutionError.hpp>
#include <vector>

using namespace lux;

struct ContextDependencies final
{
    using EngineDependencies = std::tuple<engine::EngineContext, execution::ExecutionRuntime>;
};

static_assert(engine::EngineSystemDeclaration::types<ContextDependencies>().size() == 2);
static_assert(
    engine::EngineSystemDeclaration::types<ContextDependencies>()[0] ==
    cxx::make_basic_type_info<execution::ExecutionRuntime>()
);
static_assert(
    engine::EngineSystemDeclaration::types<ContextDependencies>()[1] ==
    cxx::make_basic_type_info<engine::assets::AssetRepository>()
);

struct Input final
{
    unsigned samples = 0;

    engine::EngineResult<void> sample() noexcept
    {
        ++samples;
        return {};
    }
};

struct Resource final
{
    std::vector<unsigned>& order;
    unsigned identity;
    unsigned attempts = 0;
    bool refuse = false;

    exec::task<engine::EngineResult<void>> close() noexcept
    {
        ++attempts;
        if (refuse)
        {
            co_return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }
        order.push_back(identity);
        co_return engine::EngineResult<void>{};
    }
};

int main()
{
    // No product assembly or graphics dependency is needed to drive and close an Engine.
    auto minimal = engine::Engine::create({1, 16, {8}}, 16);
    assert(minimal && (*minimal)->step() && (*minimal)->close());

    std::vector<unsigned> order;
    Resource first{order, 1}, second{order, 2};
    Input input;
    auto engine = engine::Engine::create({1, 16, {8}}, 16);
    assert(engine);
    assert(((*engine)->registerOperation<engine::EEngineOperationPhase::POLL, &Input::sample>(input)));
    assert(((*engine)->registerClose<&Resource::close>(first, "first", {}, {"lux.engine.scenes"})));
    assert(((*engine)->registerClose<&Resource::close>(second, "second", {"first"}, {"lux.engine.scenes"})));
    // Rejected metadata is not published, and does not replace already borrowed operations.
    assert(!((*engine)->registerClose<&Resource::close>(second, "first")));
    assert(!((*engine)->registerClose<&Resource::close>(second, "missing", {"not-registered"})));
    assert(!((*engine)->registerClose<&Resource::close>(second, "cycle", {"first"}, {"first"})));
    assert((*engine)->step() && input.samples == 1);
    assert((*engine)->step() && input.samples == 2);
    assert((*engine)->close());
    assert((order == std::vector<unsigned>{1, 2}));

    std::vector<unsigned> retry_order;
    Resource completed{retry_order, 1}, recovering{retry_order, 2, 0, true}, dependent{retry_order, 3};
    auto retry_engine = engine::Engine::create({1, 16, {8}}, 16);
    assert(retry_engine);
    unsigned errors = 0;
    auto failure = object::LuxObject::connect(
        retry_engine->get(),
        &engine::Engine::failureReported,
        [&](const error::Error&) noexcept { ++errors; }
    );
    assert(failure);
    assert(((*retry_engine)->registerClose<&Resource::close>(completed, "completed", {}, {"lux.engine.scenes"})));
    assert((
        (*retry_engine)->registerClose<&Resource::close>(recovering, "recovering", {"completed"}, {"lux.engine.scenes"})
    ));
    assert((
        (*retry_engine)->registerClose<&Resource::close>(dependent, "dependent", {"recovering"}, {"lux.engine.scenes"})
    ));
    (*retry_engine)->requestClose();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!(*retry_engine)->closeFailed())
    {
        assert(std::chrono::steady_clock::now() < deadline && (*retry_engine)->step());
    }
    assert(completed.attempts == 1 && recovering.attempts == 1 && dependent.attempts == 0 && errors == 1);
    assert((*retry_engine)->step() && recovering.attempts == 1); // Failure does not automatically retry.
    recovering.refuse = false;
    const auto recovered = (*retry_engine)->close();
    assert(!recovered && (*retry_engine)->closed()); // Preserve the original failure despite successful cleanup.
    assert(completed.attempts == 1 && recovering.attempts == 2 && dependent.attempts == 1);
    assert((retry_order == std::vector<unsigned>{1, 2, 3}));
}
