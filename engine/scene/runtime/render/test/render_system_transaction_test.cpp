#include <lux/engine/function/render/client/features/light/LightOperation.hpp>
#include <lux/engine/scene/RenderSystem.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{
    class FakeStage final : public lux::scene::RenderSyncStage
    {
    public:
        FakeStage(int value, std::vector<int>& trace) : id(value), log(&trace)
        {
        }

        [[nodiscard]] bool hasPendingChanges() const noexcept override
        {
            return pending;
        }

        void requestFullSync() noexcept override
        {
            ++request_count;
            pending = true;
        }

        [[nodiscard]] lux::scene::ERenderSyncPrepareResult
        prepare(lux::render::RenderProgramBuilder<>& builder) noexcept override
        {
            ++prepare_count;
            log->push_back(id);
            if (!pending)
            {
                return lux::scene::ERenderSyncPrepareResult::NO_CHANGES;
            }
            if (fail)
            {
                return lux::scene::ERenderSyncPrepareResult::FAILED;
            }
            if (!emit_command)
            {
                return lux::scene::ERenderSyncPrepareResult::PREPARED_NO_COMMANDS;
            }
            builder.push(
                lux::render::opcodes::CommandOp,
                static_cast<lux::render::TypeId>(500U + id),
                lux::render::RemoveLightPayload{}
            );
            return lux::scene::ERenderSyncPrepareResult::PREPARED_COMMANDS;
        }

        void commitPrepared() noexcept override
        {
            ++commit_count;
            log->push_back(100 + id);
            pending = false;
        }

        void discardPrepared() noexcept override
        {
            ++discard_count;
            log->push_back(200 + id);
        }

        int id{};
        std::vector<int>* log{};
        bool pending{true};
        bool fail{false};
        bool emit_command{true};
        int prepare_count{};
        int commit_count{};
        int discard_count{};
        int request_count{};
    };

    std::unique_ptr<FakeStage> makeStage(int id, std::vector<int>& log, FakeStage*& observer)
    {
        auto stage = std::make_unique<FakeStage>(id, log);
        observer = stage.get();
        return stage;
    }
}

int main()
{
    using namespace lux;

    assert(!scene::RenderSystem::create({}));
    scene::RenderSystem::StageList null_stages;
    null_stages.push_back(nullptr);
    assert(!scene::RenderSystem::create(std::move(null_stages)));

    std::vector<int> log;
    FakeStage* first{};
    FakeStage* second{};
    scene::RenderSystem::StageList stages;
    stages.push_back(makeStage(1, log, first));
    stages.push_back(makeStage(2, log, second));
    auto created = scene::RenderSystem::create(std::move(stages));
    assert(created);
    auto system = std::move(*created);
    assert(system->tryPublish() == scene::ERenderPublishResult::FULL_SYNC_PUBLISHED);
    assert((log == std::vector<int>{1, 2, 101, 102}));

    first->pending = true;
    const int prepare_before_backpressure = first->prepare_count;
    assert(system->tryPublish() == scene::ERenderPublishResult::BACKPRESSURED);
    assert(first->prepare_count == prepare_before_backpressure);

    auto channel = render::RenderProgramChannel<>::create(1U);
    auto sync = std::make_shared<render::RenderChannelSync>();
    render::RenderProgramSession session{channel, sync};
    assert(system->tryForwardUpdate(session) == scene::ERenderForwardResult::FORWARDED);
    assert(channel->requests.tryAcquireRead());
    assert(channel->requests.currentRead().kind == render::ERenderProgramKind::StateUpdate);
    assert(channel->requests.currentRead().commands.size() == 2U);

    system->requestFullSync();
    assert(first->request_count == 1 && second->request_count == 1);

    std::vector<int> no_command_log;
    FakeStage* no_command{};
    scene::RenderSystem::StageList no_command_stages;
    auto no_command_owner = makeStage(3, no_command_log, no_command);
    no_command_owner->emit_command = false;
    no_command_stages.push_back(std::move(no_command_owner));
    auto no_command_system = scene::RenderSystem::create(std::move(no_command_stages));
    assert(no_command_system);
    assert((*no_command_system)->tryPublish() == scene::ERenderPublishResult::NO_CHANGES);
    assert(no_command->commit_count == 1);
    assert((*no_command_system)->tryForwardUpdate(session) == scene::ERenderForwardResult::NO_UPDATE);

    std::vector<int> failure_log;
    FakeStage* before_failure{};
    FakeStage* failing{};
    FakeStage* after_failure{};
    scene::RenderSystem::StageList failure_stages;
    failure_stages.push_back(makeStage(4, failure_log, before_failure));
    auto failure_owner = makeStage(5, failure_log, failing);
    failure_owner->fail = true;
    failure_stages.push_back(std::move(failure_owner));
    failure_stages.push_back(makeStage(6, failure_log, after_failure));
    auto failure_system = scene::RenderSystem::create(std::move(failure_stages));
    assert(failure_system);
    assert((*failure_system)->tryPublish() == scene::ERenderPublishResult::FAILED);
    assert(before_failure->discard_count == 1);
    assert(failing->discard_count == 1);
    assert(after_failure->discard_count == 1);
    assert(after_failure->prepare_count == 0);
    return 0;
}
