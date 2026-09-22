#include <cassert>
#include <iostream>
#include <lux/engine/function/render/client/RenderControlSession.hpp>

namespace
{
struct ShaderRelease final
{
    lux::render::RenderRequest<lux::render::ShaderCompiledReply> request;
    lux::render::ShaderHandle handle;
    unsigned &destroyed;
    ~ShaderRelease()
    {
        ++destroyed;
    }

    static bool release(void *input, lux::render::RenderControlSession &session, std::size_t &commands,
                        bool retired) noexcept
    {
        auto &value = *static_cast<ShaderRelease *>(input);
        if (retired)
        {
            value.request = {};
            value.handle = {};
            return true;
        }
        if (value.request.valid())
        {
            if (!value.request.isReady())
            {
                return false;
            }
            auto result = value.request.tryResult();
            if (result)
            {
                value.handle = result->get().shader;
            }
            value.request = {};
        }
        if (value.handle.isValid())
        {
            if (!commands || !session.canSubmit())
            {
                return false;
            }
            session.destroyShader(value.handle);
            --commands;
            value.handle = {};
        }
        return true;
    }
};
} // namespace

int main()
{
    using namespace lux::render;
    auto channel = RenderControlChannel<>::create(1);
    auto sync = std::make_shared<RenderChannelSync>();
    RenderControlSession session(channel, sync);
    OperationPacket<> packet;
    const auto pop = [&] { assert(channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::VALUE); };
    const auto empty = [&] { assert(channel->requests.tryPop(packet) == lux::cxx::EQueuePopResult::EMPTY); };
    const auto reply = [&]<class Reply>(TypeId type, Reply value) {
        auto *slot = channel->responses.tryBeginWrite();
        assert(slot);
        slot->clear_keep_capacity();
        slot->payload.resize(sizeof(Reply));
        std::memcpy(slot->payload.data(), &value, sizeof(Reply));
        slot->replies.push_back({.type_id = type, .payload_size = sizeof(Reply), .request_id = packet.requestId()});
        assert(channel->responses.publishWrite());
        assert(session.pumpReplies(1) == 1);
    };
    const RenderSceneId scene{1, 7};
    auto parent = session.adoptScene(scene);
    auto receipt = parent.receipt();
    auto child = session.adoptView(scene, {3, 5});
    OperationPacket<> occupied;
    assert(channel->requests.tryPush(std::move(occupied)) == lux::cxx::EQueuePushResult::ACCEPTED);
    parent = {};
    assert(session.pendingSceneReleases() == 1);
    assert(!*session.flushDeferredReleases(1));
    pop();
    // Prove the child guard with available capacity and a nonzero budget.
    assert(!*session.flushDeferredReleases(1));
    empty();
    child = {};
    assert(!*session.flushDeferredReleases(1));
    pop();
    reply(type_ids::ReplyGenericOk, GenericOkReply{});
    assert(session.pendingViewReleases() == 0);
    assert(!*session.flushDeferredReleases(1));
    assert(receipt.status().state == ESceneResourceState::RELEASING);
    pop();
    reply(type_ids::ReplyGenericOk, GenericOkReply{});
    assert(receipt.status().state == ESceneResourceState::RETIRED);
    assert(session.pendingSceneReleases() == 0);
    assert(*session.flushDeferredReleases(1));
    empty();

    // An admitted creation remains owned even when the CPU system disappears
    // before the real asynchronous response arrives.
    auto pending = session.prepareScene({.name = "late scene"}, {});
    receipt = pending.receipt();
    assert(pending && !pending.id().isValid());
    assert(session.maintainScenes(0) == 0);
    empty();
    assert(session.maintainScenes(1) == 1);
    pending = {};
    assert(session.maintainScenes(1) == 0);
    assert(receipt.status().state == ESceneResourceState::CREATING);
    pop();
    reply(type_ids::ReplySceneCreated, SceneCreatedReply{{2, 8}, {}});
    assert(session.maintainScenes(1) == 1);
    pop();
    reply(type_ids::ReplyGenericOk, GenericOkReply{});
    assert(receipt.status().state == ESceneResourceState::RETIRED);

    // Late successful Feature attachment cannot retain a CPU owner or orphan
    // the newly-created GPU feature. Its Scene remains the release authority.
    std::vector<SceneFeatureAttachment> features;
    features.push_back({17, 3, {}, {}});
    auto feature_owner = session.prepareScene({.name = "late feature"}, std::move(features));
    receipt = feature_owner.receipt();
    assert(session.maintainScenes(1) == 1);
    pop();
    reply(type_ids::ReplySceneCreated, SceneCreatedReply{{4, 9}, {}});
    assert(session.maintainScenes(1) == 1);
    feature_owner = {};
    pop();
    reply(type_ids::ReplyFeatureAdded, FeatureAddedReply{{2, 1}, {}});
    assert(session.maintainScenes(1) == 1);
    pop();
    reply(type_ids::ReplyGenericOk, GenericOkReply{});
    assert(receipt.status().state == ESceneResourceState::RETIRED);

    // Result observation does not keep a resource alive. An accepted Program's
    // use does, and destroying that use never submits a release itself.
    auto owner = session.adoptScene({5, 2});
    receipt = owner.receipt();
    auto program_use = owner.retain();
    owner = {};
    assert(session.maintainScenes(1) == 0);
    empty();
    program_use = {};
    empty();
    assert(session.maintainScenes(1) == 1);
    pop();
    reply(type_ids::ReplyGenericOk, GenericOkReply{});
    assert(receipt.status().state == ESceneResourceState::RETIRED);

    unsigned destroyed{};
    auto payload = std::shared_ptr<ShaderRelease>(new ShaderRelease{{}, {}, destroyed});
    auto use = session.retainResource(payload, &ShaderRelease::release);
    assert(use);
    auto in_program = use->retain();
    // Test transports inject the exact late reply; no GPU qualification is
    // inferred from this bounded queue/ownership test.
    payload->request = session.compileShader(std::span<const std::byte>{});
    pop();
    *use = {};
    payload.reset();
    assert(session.maintainResources(1) == 0 && destroyed == 0);
    reply(type_ids::ReplyShaderCompiled, ShaderCompiledReply{{9, 4}, 0});
    assert(channel->requests.tryPush(std::move(occupied)) == lux::cxx::EQueuePushResult::ACCEPTED);
    in_program = {};
    assert(session.pendingResourceReleases() == 1);
    assert(session.maintainResources(1) == 0 && destroyed == 0);
    pop();
    assert(session.maintainResources(1) == 1 && destroyed == 1);
    pop();
    assert(packet.command.type_id == type_ids::DestroyShader);
    assert(session.pendingResourceReleases() == 0);

    auto terminal_payload = std::shared_ptr<ShaderRelease>(new ShaderRelease{{}, {10, 5}, destroyed});
    auto terminal_use = session.retainResource(terminal_payload, &ShaderRelease::release);
    assert(terminal_use);
    terminal_payload.reset();
    *terminal_use = {};
    auto stopped = session.adoptScene({6, 3});
    receipt = stopped.receipt();
    stopped = {};
    sync->requestStop();
    assert(session.maintainScenes(1) == 0);
    assert(!receipt.status().failure.ok());
    assert(receipt.status().state != ESceneResourceState::RETIRED);
    assert(session.maintainResources(1) == 0 && destroyed == 1);
    session.retireScenesAfterBackendStopped();
    assert(receipt.status().state == ESceneResourceState::RETIRED);
    assert(!receipt.status().failure.ok());
    assert(session.maintainResources(0) == 0 && destroyed == 2);
    assert(session.pendingResourceReleases() == 0);
    std::cout << "PASS release records: saturation, child order, late create/attach, Program use, terminal proof\n";
}
