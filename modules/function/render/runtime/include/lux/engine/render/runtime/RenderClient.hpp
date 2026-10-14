#pragma once

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

    struct RenderReply final
    {
        ReplyHeader header;
        SessionIdentity session{};
        SceneId scene;
        ViewId view;
        TargetId target;
        EffectId effect;
        OwnedWireBytes data; // pins reply + returned input + its one budget lease
        std::uint64_t target_epoch = 0;
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
        [[nodiscard]] RenderResult<std::optional<RenderReply>> poll(RequestId request) noexcept;
        // Also reclaims returned owners for abandoned requests when no request is pending.
        std::size_t collectReplies() noexcept;
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
