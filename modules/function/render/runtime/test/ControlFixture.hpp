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
    AttachmentId attachment;
    TargetDesc description;
    RenderPayload parameters;
    std::vector<FeaturePreset> features;
};
enum class ETestOutcome { SUCCESS, FAILED, CANCELLED, OUTCOME_UNKNOWN };
struct TestReplySummary final { RequestId request; ETestOutcome outcome = ETestOutcome::FAILED; };
struct TestCompletion final
{
    ipc::IpcMessage owner;
    TestReplySummary header;
    RenderError error;
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    AttachmentId attachment;
    std::uint64_t target_epoch = 0;
    ETestCommand command = ETestCommand::CATALOG;
    bool active() const noexcept { return !owner.empty(); }
    std::span<const std::byte> bytes() const noexcept
    {
        return command == ETestCommand::READBACK && header.outcome == ETestOutcome::SUCCESS ?
            owner.get<Readback::Response>()->pixels.bytes().view() : std::span<const std::byte>{};
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
            return send<CreateView>({input.scene}, input.operation);
        case ETestCommand::REMOVE_VIEW: return send<RemoveView>({input.scene, input.view}, input.operation);
        case ETestCommand::CREATE_TARGET: return send<CreateTarget>({input.description}, input.operation);
        case ETestCommand::RESIZE_TARGET: return send<ResizeTarget>({input.target, input.description}, input.operation);
        case ETestCommand::REMOVE_EFFECT: return send<RemoveEffect>({input.effect}, input.operation);
        case ETestCommand::ATTACH:
            return send<Attach>({input.scene, input.view, input.target, input.effect, input.parameters}, input.operation);
        case ETestCommand::DETACH:
            return send<Detach>(input.attachment, input.operation);
        case ETestCommand::READBACK:
            return send<Readback>({input.attachment, {}}, input.operation);
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
                if (!peer_.trySend(commandTag(found->id), id.value, message)) return lux::cxx::unexpected(RenderError{kRuntimeState});
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (;;)
                {
                    if (std::chrono::steady_clock::now() >= deadline) std::abort();
                    std::uint64_t correlation = 0;
                    const auto status = peer_.tryReceive(correlation, message);
                    if (status == ipc::EIpcReceive::CLOSED) std::abort();
                    if (status != ipc::EIpcReceive::RECEIVED) { std::this_thread::yield(); continue; }
                    auto& response = message.get<ListFeatures::Response>();
                    if (correlation != id.value || !response) std::abort();
                    features_ = std::move(*response);
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
        std::uint64_t correlation = 0;
        const auto status = peer_.tryReceive(correlation, output.owner);
        if (status != ipc::EIpcReceive::RECEIVED) return status;
        output.header.request = RequestId{correlation};
        const auto found = std::ranges::find(pending_, output.header.request, &Pending::id);
        if (found == pending_.end()) std::abort();
        output.command = found->command;
        pending_.erase(found);
        if (output.owner.typeHash() == lux::cxx::type_hash<RenderError>())
        {
            output.error = output.owner.get<RenderError>();
            return status;
        }
        const auto read = [&]<class T>() -> const T* {
            const auto& result = output.owner.get<RenderResult<T>>();
            if (!result)
            {
                output.error = result.error();
                output.header.outcome = result.error().type == kCommandCancelled ? ETestOutcome::CANCELLED :
                    result.error().type == kOutcomeUnknown ? ETestOutcome::OUTCOME_UNKNOWN : ETestOutcome::FAILED;
                return nullptr;
            }
            output.header.outcome = ETestOutcome::SUCCESS;
            return &*result;
        };
        switch (output.command)
        {
        case ETestCommand::CATALOG:
            if (const auto* result = read.operator()<CommandList>()) commands_ = *result;
            break;
        case ETestCommand::CREATE_SCENE:
            if (const auto* result = read.operator()<SceneCreated>()) output.scene = result->scene;
            break;
        case ETestCommand::CREATE_VIEW:
            if (const auto* result = read.operator()<ViewCreated>()) output.view = result->view;
            break;
        case ETestCommand::CREATE_TARGET:
        case ETestCommand::RESIZE_TARGET:
            if (const auto* result = read.operator()<TargetCreated>())
            {
                output.target = result->target;
                output.target_epoch = result->epoch;
            }
            break;
        case ETestCommand::CREATE_EFFECT:
            if (const auto* result = read.operator()<EffectCreated>()) output.effect = result->effect;
            break;
        case ETestCommand::READBACK: (void)read.operator()<ReadbackResult>(); break;
        case ETestCommand::ATTACH:
            if (const auto* result = read.operator()<AttachmentCreated>()) output.attachment = result->attachment;
            break;
        default: (void)read.operator()<Ack>(); break;
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
        if (!peer_.trySend(commandTag(id), request.value, message)) return lux::cxx::unexpected(RenderError{kRuntimeState});
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
