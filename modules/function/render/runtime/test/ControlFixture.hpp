#pragma once
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/runtime/Commands.hpp>
#include "ServerFixture.hpp"
#include "../pinclude/TypedProtocol.hpp"
#include <variant>

// Only migrates existing behavioral fixtures. Production has no command enum or completion cache.
using namespace lux::render;
enum class ETestCommand { READY, CREATE_SCENE, REMOVE_SCENE, CREATE_VIEW, REMOVE_VIEW, CREATE_TARGET,
    RESIZE_TARGET, CREATE_EFFECT, REMOVE_EFFECT, ATTACH, DETACH, READBACK, STOP };
struct TestCommand final
{
    ETestCommand operation = ETestCommand::READY;
    SessionIdentity session{};
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    TargetDesc description;
    std::array<float, 4> color{1, 0, 0, 1};
    std::vector<FeatureSelection> features;
    std::vector<RenderPayload> attachments;
    std::size_t reply_capacity = 64 * 1024;
};

using VTestCompletionOwner = std::variant<RenderCompletion, TypedRenderCompletion<CreateScene>,
    TypedRenderCompletion<RemoveScene>, TypedRenderCompletion<CreateView>, TypedRenderCompletion<RemoveView>,
    TypedRenderCompletion<CreateTarget>, TypedRenderCompletion<ResizeTarget>, TypedRenderCompletion<CreateEffect>,
    TypedRenderCompletion<RemoveEffect>, TypedRenderCompletion<Attach>, TypedRenderCompletion<Detach>,
    TypedRenderCompletion<Readback>, TypedRenderCompletion<Stop>>;

struct TestCompletion final
{
    VTestCompletionOwner owner;
    ReplyHeader header;
    SessionIdentity session{};
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    std::uint64_t target_epoch = 0;
    bool active() const noexcept
    {
        return std::visit([](const auto& value) { return value.active(); }, owner);
    }

    std::span<const std::byte> bytes() const noexcept
    {
        const auto* readback = std::get_if<TypedRenderCompletion<Readback>>(&owner);
        return readback ? readback->response().pixels.bytes().view() : std::span<const std::byte>{};
    }
};

class TestClient final
{
public:
    static RenderResult<std::unique_ptr<TestClient>> create(std::string_view name, RenderClientConfig config = {})
    {
        auto client = RenderClient::create(name, config);
        if (!client) return lux::cxx::unexpected(client.error());
        return std::unique_ptr<TestClient>(new TestClient(std::move(*client), std::string(name)));
    }

    RenderResult<RequestId> submit(const TestCommand& command)
    {
        switch (command.operation)
        {
        case ETestCommand::READY: return client_->hello();
        case ETestCommand::CREATE_SCENE: return send<CreateScene>({}, command);
        case ETestCommand::REMOVE_SCENE: return send<RemoveScene>({command.scene}, command);
        case ETestCommand::CREATE_VIEW: return send<CreateView>({command.scene, command.color}, command);
        case ETestCommand::REMOVE_VIEW: return send<RemoveView>({command.scene, command.view}, command);
        case ETestCommand::CREATE_TARGET: return send<CreateTarget>({command.description}, command);
        case ETestCommand::RESIZE_TARGET: return send<ResizeTarget>({command.target, command.description}, command);
        case ETestCommand::REMOVE_EFFECT: return send<RemoveEffect>({command.effect}, command);
        case ETestCommand::ATTACH:
            return send<Attach>({command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::DETACH:
            return send<Detach>({command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::READBACK:
            return send<Readback>({command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::STOP: return send<Stop>({}, command);
        case ETestCommand::CREATE_EFFECT:
            return send<CreateEffect>({command.description, command.features, command.attachments}, command);
        }
        return lux::cxx::unexpected(RenderError{kRuntimeUnsupported});
    }

    RenderResult<ERenderReceive> tryReceive(TestCompletion& output)
    {
        if (output.active()) return lux::cxx::unexpected(RenderError{kRuntimeState});
        if (raw_pending_)
        {
            const auto status = raw_->tryReceive(raw_reply_);
            if (status == lux::platform::ipc::EIpcReceive::EMPTY) return ERenderReceive::EMPTY;
            if (status != lux::platform::ipc::EIpcReceive::RECEIVED) std::abort();
            const auto frame = raw_reply_.message.metadata<runtime_detail::ReplyMetadata>();
            output.header = {frame.request, frame.outcome,
                raw_reply_.message.get<runtime_detail::ValueEnvelope<RenderError>>().value};
            output.session = frame.session;
            raw_pending_ = false;
            raw_reply_ = {};
            return ERenderReceive::RECEIVED;
        }
        RenderCompletion completion;
        auto result = client_->tryReceive(completion);
        if (!result || *result != ERenderReceive::RECEIVED) return result;
        output.header = completion.header;
        output.session = completion.session;
        if (completion.header.outcome != ECommandOutcome::SUCCESS || completion.command().isNull())
        {
            output.owner = std::move(completion);
            return result;
        }
        bool matched = false;
        RenderError error;
        std::apply([&](const auto&... binding) {
            ([&] {
                if (!binding || completion.command() != binding->command()) return;
                auto typed = std::move(completion).take(*binding);
                if (!typed) { error = typed.error(); return; }
                using Owned = std::remove_cvref_t<decltype(*typed)>;
                const auto& response = typed->response();
                if constexpr (requires { response.scene; }) output.scene = response.scene;
                if constexpr (requires { response.view; }) output.view = response.view;
                if constexpr (requires { response.target; }) output.target = response.target;
                if constexpr (requires { response.epoch; }) output.target_epoch = response.epoch;
                if constexpr (requires { response.effect; }) output.effect = response.effect;
                output.owner.template emplace<Owned>(std::move(*typed));
                matched = true;
            }(), ...);
        }, bindings_);
        if (error.type) return lux::cxx::unexpected(error);
        if (!matched) return lux::cxx::unexpected(RenderError{kRuntimeIdentity});
        return result;
    }

    RenderResult<void> abandon(RequestId id) { return client_->abandon(id); }
    std::size_t retainedBytes() const { return client_->retainedBytes(); }

private:
    explicit TestClient(std::unique_ptr<RenderClient> client, std::string name)
        : client_(std::move(client)), name_(std::move(name)) {}

    template <RenderCommand C>
    RenderResult<RequestId> send(typename C::Request payload, const TestCommand& command)
    {
        auto& binding = std::get<std::optional<BoundCommand<C>>>(bindings_);
        if (!binding)
        {
            auto value = client_->bind<C>();
            if (!value) return lux::cxx::unexpected(value.error());
            binding.emplace(std::move(*value));
        }
        if (command.session != client_->session())
        {
            // Deliberately hostile raw peer, not a mutable Client binding.
            if (!raw_)
            {
                auto peer = lux::platform::ipc::IpcCore::instance().createRequester(name_);
                if (!peer) std::abort();
                raw_.emplace(std::move(*peer));
            }
            const RequestId id{UINT32_MAX - 1, 1};
            lux::platform::ipc::IpcMessage message;
            message.emplace<runtime_detail::ValueEnvelope<typename C::Request>>(
                runtime_detail::BudgetLease{}, std::move(payload));
            message.setMetadata(runtime_detail::RequestMetadata{
                command.session, id, binding->command(), command.reply_capacity});
            if (!raw_->trySend(message)) std::abort();
            raw_pending_ = true;
            return id;
        }
        return client_->submit(*binding, payload, command.reply_capacity);
    }

    std::unique_ptr<RenderClient> client_;
    std::string name_;
    std::tuple<std::optional<BoundCommand<CreateScene>>, std::optional<BoundCommand<RemoveScene>>,
        std::optional<BoundCommand<CreateView>>, std::optional<BoundCommand<RemoveView>>,
        std::optional<BoundCommand<CreateTarget>>, std::optional<BoundCommand<ResizeTarget>>,
        std::optional<BoundCommand<CreateEffect>>, std::optional<BoundCommand<RemoveEffect>>,
        std::optional<BoundCommand<Attach>>, std::optional<BoundCommand<Detach>>,
        std::optional<BoundCommand<Readback>>, std::optional<BoundCommand<Stop>>> bindings_;
    std::optional<lux::platform::ipc::IpcRequester> raw_;
    lux::platform::ipc::IpcReply raw_reply_;
    bool raw_pending_ = false;
};
