#pragma once
#include <lux/engine/render/runtime/Operations.hpp>

// Only migrates existing behavioral fixtures. Production has no command enum, catch-all packet or reply cache.
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

struct TestCompletion final
{
    RenderCompletion value;
    ReplyHeader header;
    SessionIdentity session{};
    std::optional<DataSchemaDescriptor> schema;
    bool active() const noexcept { return value.active(); }
    std::span<const std::span<const std::byte>> attachments() const noexcept { return value.attachments(); }
    SceneId scene;
    ViewId view;
    TargetId target;
    EffectId effect;
    std::uint64_t target_epoch = 0;
    std::span<const std::byte> bytes() const noexcept
    {
        const auto views = attachments();
        return views.empty() ? value.bytes() : views[0];
    }
};

class TestClient final
{
public:
    static RenderResult<std::unique_ptr<TestClient>> create(std::string_view name, RenderClientConfig config = {})
    {
        auto client = RenderClient::create(name, config);
        if (!client) return lux::cxx::unexpected(client.error());
        return std::unique_ptr<TestClient>(new TestClient(std::move(*client)));
    }
    RenderResult<RequestId> submit(const TestCommand& command)
    {
        switch (command.operation)
        {
        case ETestCommand::READY: return client_->hello();
        case ETestCommand::CREATE_SCENE: return send<EmptyRequest, SceneCreated>(operations::createScene(), EmptyRequest{}, command);
        case ETestCommand::REMOVE_SCENE: return send<SceneRequest, void>(operations::removeScene(), SceneRequest{command.scene}, command);
        case ETestCommand::CREATE_VIEW: return send<CreateViewRequest, ViewCreated>(operations::createView(), CreateViewRequest{command.scene, command.color}, command);
        case ETestCommand::REMOVE_VIEW: return send<ViewRequest, void>(operations::removeView(), ViewRequest{command.scene, command.view}, command);
        case ETestCommand::CREATE_TARGET: return send<CreateTargetRequest, TargetCreated>(operations::createTarget(), CreateTargetRequest{command.description}, command);
        case ETestCommand::RESIZE_TARGET: return send<ResizeTargetRequest, TargetCreated>(operations::resizeTarget(), ResizeTargetRequest{command.target, command.description}, command);
        case ETestCommand::REMOVE_EFFECT: return send<EffectRequest, void>(operations::removeEffect(), EffectRequest{command.effect}, command);
        case ETestCommand::ATTACH: return send<AttachmentRequest, void>(operations::attach(), AttachmentRequest{command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::DETACH: return send<AttachmentRequest, void>(operations::detach(), AttachmentRequest{command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::READBACK: return send<AttachmentRequest, ReadbackResult>(operations::readback(), AttachmentRequest{command.scene, command.view, command.target, command.effect}, command);
        case ETestCommand::STOP: return send<EmptyRequest, void>(operations::stop(), EmptyRequest{}, command);
        case ETestCommand::CREATE_EFFECT:
        {
            auto selections = encodeFeatureSelections(command.features);
            if (!selections) return lux::cxx::unexpected(selections.error());
            return send<CreateEffectRequest, EffectCreated>(operations::createEffect(),
                {command.description, std::move(*selections)}, command);
        }
        }
        return lux::cxx::unexpected(RenderError{kRuntimeUnsupported});
    }
    RenderResult<ERenderReceive> tryReceive(TestCompletion& output)
    {
        auto result = client_->tryReceive(output.value);
        output.header = output.value.header;
        output.session = output.value.session;
        output.schema = output.value.schema;
        if (!result || *result != ERenderReceive::RECEIVED ||
            output.header.outcome != EOperationOutcome::SUCCESS || !output.schema) return result;
        BorrowedWireView wire{*output.schema, output.value.bytes(), output.attachments()};
        if (wire.schema.type == DataSchema<SceneCreated>::describe().type)
        {
            auto value = decodeWireBytes<SceneCreated>(wire, {1024, SIZE_MAX});
            if (!value) return lux::cxx::unexpected(value.error());
            output.scene = value->scene;
        }
        if (wire.schema.type == DataSchema<ViewCreated>::describe().type)
        {
            auto value = decodeWireBytes<ViewCreated>(wire, {1024, SIZE_MAX});
            if (!value) return lux::cxx::unexpected(value.error());
            output.view = value->view;
        }
        if (wire.schema.type == DataSchema<TargetCreated>::describe().type)
        {
            auto value = decodeWireBytes<TargetCreated>(wire, {1024, SIZE_MAX});
            if (!value) return lux::cxx::unexpected(value.error());
            output.target = value->target; output.target_epoch = value->epoch;
        }
        if (wire.schema.type == DataSchema<EffectCreated>::describe().type)
        {
            auto value = decodeWireBytes<EffectCreated>(wire, {1024, SIZE_MAX});
            if (!value) return lux::cxx::unexpected(value.error());
            output.effect = value->effect;
        }
        return result;
    }
    RenderResult<void> abandon(RequestId id) { return client_->abandon(id); }
    std::size_t retainedBytes() const { return client_->retainedBytes(); }
private:
    explicit TestClient(std::unique_ptr<RenderClient> client) : client_(std::move(client)) {}
    template <WireData T, typename Reply>
    RenderResult<RequestId> send(const OperationDescriptor& descriptor, const T& payload, const TestCommand& command)
    {
        auto binding = client_->bind<T, Reply>(descriptor);
        if (!binding) return lux::cxx::unexpected(binding.error());
        binding->binding.session = command.session; // explicit hostile/stale-session fixture
        return client_->submit(*binding, payload, command.attachments, command.reply_capacity);
    }
    std::unique_ptr<RenderClient> client_;
};
