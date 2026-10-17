#pragma once
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include "CameraFixture.hpp"
#include "ServerFixture.hpp"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <thread>

using namespace lux::render;
namespace ipc = lux::platform::ipc;
using ERenderReceive = ipc::EIpcReceive; // Fixture status spelling only, no production Client API.

// Application test driver: it deliberately owns its correlation records outside IPC/Render.
enum class ETestCommand { CATALOG, CREATE_SCENE, REMOVE_SCENE, CREATE_VIEW, REMOVE_VIEW, CREATE_TARGET,
    RESIZE_TARGET, CREATE_EFFECT, REMOVE_EFFECT, ATTACH, DETACH, READBACK, STOP };
struct FeaturePreset final { std::string name, instance; ConfigValue config; };
struct TestCommand final
{
    ETestCommand operation = ETestCommand::CATALOG;
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    TargetDesc description;
    std::array<float, 4> color{1, 0, 0, 1};
    std::vector<FeaturePreset> features;
};
struct TestCompletion final
{
    ipc::IpcMessage owner;
    ReplyHeader header;
    RenderError error;
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    std::uint64_t target_epoch = 0;
    ETestCommand command = ETestCommand::CATALOG;
    bool active() const noexcept { return !owner.empty(); }
    std::span<const std::byte> bytes() const noexcept
    {
        return command == ETestCommand::READBACK && header.outcome == ECommandOutcome::SUCCESS ?
            owner.get<ReadbackResult>().pixels.bytes().view() : std::span<const std::byte>{};
    }
};
class TestClient final
{
public:
    static RenderResult<std::unique_ptr<TestClient>> create(std::string_view name, CommandId catalog)
    {
        auto peer = ipc::IpcCore::instance().createRequester(name);
        if (!peer) return lux::cxx::unexpected(RenderError{kRuntimeState});
        return std::unique_ptr<TestClient>(new TestClient(std::move(*peer), catalog));
    }
    RenderResult<RequestId> submit(const TestCommand& input)
    {
        switch (input.operation)
        {
        case ETestCommand::CATALOG: return send<ListCommands>({}, input.operation, catalog_);
        case ETestCommand::CREATE_SCENE: return send<CreateScene>({}, input.operation);
        case ETestCommand::REMOVE_SCENE: return send<RemoveScene>({input.scene}, input.operation);
        case ETestCommand::CREATE_VIEW:
            return send<CreateView>({input.scene, camera_fixture::oracle(input.color)}, input.operation);
        case ETestCommand::REMOVE_VIEW: return send<RemoveView>({input.scene, input.view}, input.operation);
        case ETestCommand::CREATE_TARGET: return send<CreateTarget>({input.description}, input.operation);
        case ETestCommand::RESIZE_TARGET: return send<ResizeTarget>({input.target, input.description}, input.operation);
        case ETestCommand::REMOVE_EFFECT: return send<RemoveEffect>({input.effect}, input.operation);
        case ETestCommand::ATTACH:
            return send<Attach>({input.scene, input.view, input.target, input.effect}, input.operation);
        case ETestCommand::DETACH:
            return send<Detach>({input.scene, input.view, input.target, input.effect}, input.operation);
        case ETestCommand::READBACK:
            return send<Readback>({{input.scene, input.view, input.target, input.effect}, {}}, input.operation);
        case ETestCommand::STOP: return send<Stop>({}, input.operation);
        case ETestCommand::CREATE_EFFECT:
        {
            if (features_.features.empty())
            {
                auto found = std::ranges::find(commands_.commands, ListFeatures::name, &CommandInfo::name);
                if (found == commands_.commands.end()) return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
                const RequestId id{next_++};
                ipc::IpcMessage message;
                message.emplace<ListFeatures::Request>();
                message.setMetadata(RequestHeader{id, found->id});
                if (!peer_.trySend(message)) return lux::cxx::unexpected(RenderError{kRuntimeState});
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (;;)
                {
                    if (std::chrono::steady_clock::now() >= deadline) std::abort();
                    const auto status = peer_.tryReceive(message);
                    if (status == ipc::EIpcReceive::CLOSED) std::abort();
                    if (status != ipc::EIpcReceive::RECEIVED) { std::this_thread::yield(); continue; }
                    const auto header = message.metadata<ReplyHeader>();
                    if (header.request != id || header.outcome != ECommandOutcome::SUCCESS) std::abort();
                    features_ = std::move(message.get<FeatureList>());
                    break;
                }
            }
            CreateEffectRequest request{input.description};
            for (const auto& selected : input.features)
            {
                const auto found = std::ranges::find(features_.features, selected.name, &FeatureInfo::name);
                if (found == features_.features.end()) return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
                request.features.push_back({found->id, selected.instance, selected.config});
            }
            return send<CreateEffect>(std::move(request), input.operation);
        }
        }
        std::abort();
    }
    RenderResult<ERenderReceive> tryReceive(TestCompletion& output)
    {
        if (output.active()) return lux::cxx::unexpected(RenderError{kRuntimeState});
        const auto status = peer_.tryReceive(output.owner);
        if (status != ipc::EIpcReceive::RECEIVED) return status;
        output.header = output.owner.metadata<ReplyHeader>();
        const auto found = std::ranges::find(pending_, output.header.request, &Pending::id);
        if (found == pending_.end()) std::abort();
        output.command = found->command;
        pending_.erase(found);
        if (output.header.outcome != ECommandOutcome::SUCCESS)
        {
            output.error = output.owner.get<RenderError>();
            return status;
        }
        switch (output.command)
        {
        case ETestCommand::CATALOG: commands_ = output.owner.get<CommandList>(); break;
        case ETestCommand::CREATE_SCENE: output.scene = output.owner.get<SceneCreated>().scene; break;
        case ETestCommand::CREATE_VIEW: output.view = output.owner.get<ViewCreated>().view; break;
        case ETestCommand::CREATE_TARGET:
        case ETestCommand::RESIZE_TARGET:
            output.target = output.owner.get<TargetCreated>().target;
            output.target_epoch = output.owner.get<TargetCreated>().epoch;
            break;
        case ETestCommand::CREATE_EFFECT: output.effect = output.owner.get<EffectCreated>().effect; break;
        default: break;
        }
        return status;
    }
private:
    TestClient(ipc::IpcRequester peer, CommandId catalog) : peer_(std::move(peer)), catalog_(catalog) {}
    template <RenderCommand C>
    RenderResult<RequestId> send(typename C::Request input, ETestCommand operation, CommandId id = {})
    {
        if (id.isNull())
        {
            const auto found = std::ranges::find(commands_.commands, C::name, &CommandInfo::name);
            if (found == commands_.commands.end()) return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
            id = found->id;
        }
        const RequestId request{next_++};
        ipc::IpcMessage message;
        message.emplace<typename C::Request>(std::move(input));
        message.setMetadata(RequestHeader{request, id});
        if (!peer_.trySend(message)) return lux::cxx::unexpected(RenderError{kRuntimeState});
        pending_.push_back({request, operation});
        return request;
    }
    struct Pending final { RequestId id; ETestCommand command; };
    ipc::IpcRequester peer_;
    CommandId catalog_;
    CommandList commands_;
    FeatureList features_;
    std::uint64_t next_ = 1;
    std::vector<Pending> pending_; // Only this test application's reply expectations, never library state.
};
