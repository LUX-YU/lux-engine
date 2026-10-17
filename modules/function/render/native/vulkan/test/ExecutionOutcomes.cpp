#include "Support.hpp"
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/Readback.hpp>
#include <array>
#include <string_view>

using namespace foundation_test;

namespace
{
    bool hold_observation = false;
    unsigned fence_queries = 0;
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* name) noexcept
    {
        if (std::string_view(name) == "fence_status")
        {
            ++fence_queries;
            if (hold_observation) return VK_NOT_READY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char*, VkResult result) noexcept { return result; }

    void destroyed(const char*) noexcept {}
}

int main()
{
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    native_fixture::Config config{16, 16, 0, 0};
    auto assembly = take(native_fixture::assemble(config));
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly.definition, assembly.analysis,
        assembly.external_bindings, native_fixture::programs(config), native_fixture::target(config))));
    auto& queue = session->queue();
    const auto capacity = queue.capacity();
    std::vector<ExecutionContext*> contexts;
    for (unsigned i = 0; i <= capacity; ++i)
        contexts.push_back(take(session->prepare(code, native_fixture::target(config))));
    auto& first = *contexts.front();
    {
        auto batch = take(queue.begin());
        CHECK(take(first.execute()) == EExecutionOutcome::RECORDING_BUSY);
        CHECK(!first.lastUse());
    }
    hold_observation = true;
    CHECK(take(first.execute()) == EExecutionOutcome::EXECUTED);
    const auto accepted = first.lastUse();
    CHECK(take(first.execute()) == EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING);
    CHECK(first.lastUse()->serial() == accepted->serial() && &first.lastUse()->owner() == &accepted->owner());
    for (unsigned i = 1; i < capacity; ++i)
        CHECK(take(contexts[i]->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(contexts.back()->execute()) == EExecutionOutcome::QUEUE_FULL);
    CHECK(!contexts.back()->lastUse());
    hold_observation = false;
    CHECK(take(queue.wait(*queue.lastSubmitted(), UINT64_MAX)));

    // A has completed; B is pending and observation is deliberately held. A may record again.
    // There is exactly one Fence probe, not completed() plus begin() probing the same frontier twice.
    {
        auto batch = take(queue.begin());
        take(std::move(batch).submit());
    }
    hold_observation = true;
    fence_queries = 0;
    CHECK(take(first.execute()) == EExecutionOutcome::EXECUTED);
    CHECK(fence_queries == 1);
    hold_observation = false;
    CHECK(take(queue.wait(*first.lastUse(), UINT64_MAX)));

    unsigned readbacks = 0;
    {
        ReadbackOperation readback{first, &readbacks, [](void* value, ReadbackOperation::Result result) noexcept {
            CHECK(result && result->size() == 16 * 16 * 4);
            ++*static_cast<unsigned*>(value);
        }, nullptr, nullptr};
        readback.start();
        CHECK(readbacks == 0);
        CHECK(take(first.execute()) == EExecutionOutcome::READBACK_HELD);
        session->dispatchCompletions();
        CHECK(readbacks == 1);
    }
    const auto& progress = first.progress();
    CHECK(progress.count(EExecutionOutcome::EXECUTED) == 2);
    CHECK(progress.count(EExecutionOutcome::RECORDING_BUSY) == 1);
    CHECK(progress.count(EExecutionOutcome::PREVIOUS_SUBMISSION_PENDING) == 1);
    CHECK(progress.count(EExecutionOutcome::READBACK_HELD) == 1);
    CHECK(contexts.back()->progress().count(EExecutionOutcome::QUEUE_FULL) == 1);
    CHECK(take(contexts.back()->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(queue.wait(*contexts.back()->lastUse(), UINT64_MAX)));
    for (auto* context : contexts) CHECK(session->release(context));
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("execution outcomes=5 single_frontier_probe=1 validation_errors=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
}
