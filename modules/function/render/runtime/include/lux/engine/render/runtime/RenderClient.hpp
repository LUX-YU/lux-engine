#pragma once

#include <lux/engine/platform/ipc/IpcMessage.hpp>

#include <array>
#include <memory>
#include <string>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/protocol/Protocol.hpp>

namespace lux::render
{
    using SessionIdentity = std::array<std::uint64_t, 2>;
    enum class ERenderCommand : std::uint32_t
    {
        READY, CREATE_SCENE, REMOVE_SCENE, CREATE_VIEW, REMOVE_VIEW, CREATE_TARGET, RESIZE_TARGET,
        CREATE_EFFECT, REMOVE_EFFECT, ATTACH, DETACH, READBACK, STOP, COUNT
    };

    struct FeatureSelection final
    {
        std::string type;
        std::string instance;
        cxx::SchemaVersion version{1, 0};
        const DataSchemaDescriptor* config_schema = nullptr; // synchronous submit borrow
        OwnedWireBytes config;
    };

    // A narrow retained-owner factory. The complete vector capacity is charged,
    // even when only a small range is exposed. No caller-supplied byte discount.
    class RenderPayload final
    {
    public:
        [[nodiscard]] static RenderResult<RenderPayload> fromVector(
            std::shared_ptr<const std::vector<std::byte>> owner, std::size_t offset, std::size_t size
        ) noexcept;
        [[nodiscard]] const OwnedWireBytes& bytes() const noexcept { return bytes_; }
        [[nodiscard]] std::size_t retainedBytes() const noexcept { return retained_bytes_; }
    private:
        OwnedWireBytes bytes_;
        std::size_t retained_bytes_ = 0;
    };

    struct RenderCommand final
    {
        ERenderCommand operation = ERenderCommand::READY;
        SessionIdentity session{}; // required for every resource operation; never silently restamped
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

    enum class ERenderReceive { RECEIVED, EMPTY, CLOSED };

    struct RenderCompletion final
    {
        ReplyHeader header;
        SessionIdentity session{};
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect;
        std::uint64_t target_epoch = 0;
        RenderCompletion() noexcept = default;
        RenderCompletion(RenderCompletion&& other) noexcept;
        RenderCompletion& operator=(RenderCompletion&& other) noexcept;
        RenderCompletion(const RenderCompletion&) = delete;
        RenderCompletion& operator=(const RenderCompletion&) = delete;
        [[nodiscard]] bool active() const noexcept { return !header.request.isNull(); }
        // Borrow only while this Completion remains alive. The value owns both reply and returned input.
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    private:
        friend class RenderClient;
        platform::ipc::IpcReply backing_;

    };

    struct RenderClientConfig final
    {
        std::size_t retained_byte_limit = 64 * 1024 * 1024;
        std::size_t ring_capacity = 64;
    };

    // One long-lived logical SPSC owner; the host creates at most Main + Upload
    // per server epoch. No worker-per-request connections. No native dependency.
    class RenderClient final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderClient>>
        create(std::string_view channel_name, RenderClientConfig config = {}) noexcept;
        ~RenderClient() noexcept;
        RenderClient(const RenderClient&) = delete;
        RenderClient& operator=(const RenderClient&) = delete;
        [[nodiscard]] RenderResult<RequestId> submit(const RenderCommand& command) noexcept;
        // output must be inactive. Processes at most eight abandoned records per call.
        [[nodiscard]] RenderResult<ERenderReceive> tryReceive(RenderCompletion& output) noexcept;
        [[nodiscard]] RenderResult<void> abandon(RequestId request) noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;
    private:
        struct State;
        explicit RenderClient(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");
    [[nodiscard]] std::span<const error::ErrorDescriptor> renderRuntimeErrorDescriptors() noexcept;
}
