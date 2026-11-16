#pragma once

#include <concepts>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/execution/ResourceInput.hpp>
#include <memory>
#include <span>
#include <utility>

namespace nri
{
    struct CommandBuffer;
    struct CoreInterface;
} // namespace nri

namespace lux::rdesc
{
    struct ShaderProgramArtifact;
}

namespace lux::render::detail
{
    class DeviceSession;
}

namespace lux::render
{
    struct PassInterface;

    struct PassCompileContext final
    {
        detail::DeviceSession& device;
        const PassInterface& interface;
        std::span<const rdesc::ShaderProgramArtifact> programs;
    };

    // Frozen direct inputs only. No Server, Registry, scheduler or mutable GraphInstance lookup.
    struct PassRecordInput final
    {
        std::span<const detail::ResourceInput> resources;
        std::span<const std::byte> parameters;
        std::span<std::byte> scratch; // Exclusive to this recording operation.
    };

    enum class EPassEncodingScope : std::uint8_t
    {
        MANAGED,
        EXCLUSIVE
    };

    struct PassEncodingPolicy final
    {
        EPassEncodingScope scope = EPassEncodingScope::MANAGED;
        bool changes_bindings = false;

        [[nodiscard]] bool valid() const noexcept
        {
            return scope == EPassEncodingScope::MANAGED || scope == EPassEncodingScope::EXCLUSIVE;
        }

        [[nodiscard]] bool invalidatesBindings() const noexcept
        {
            return changes_bindings || scope == EPassEncodingScope::EXCLUSIVE;
        }
    };

    template <class State>
    concept PassRecordState = std::is_nothrow_move_constructible_v<State> && std::is_nothrow_destructible_v<State> &&
                              requires(
                                  const State& state,
                                  const PassRecordInput& input,
                                  nri::CommandBuffer& command,
                                  const nri::CoreInterface& core
                              ) {
                                  { state.record(input, command, core) } noexcept -> std::same_as<RenderResult<void>>;
                              };

    // Immutable device-compiled operation. Record may only encode into the exclusive CB/scratch;
    // no submission, GPU wait, Shader compilation, Registry mutation or unbounded allocation.
    class PassEncoding final
    {
        using Delete = void (*)(void*) noexcept;
        using RecordResult = RenderResult<void>;
        using Record =
            RecordResult (*)(const void*, const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&) noexcept;

    public:
        PassEncoding() noexcept = default;

        PassEncoding(PassEncoding&& other) noexcept
            : state_(std::move(other.state_)), record_(std::exchange(other.record_, nullptr)), policy_(other.policy_)
        {
        }

        PassEncoding& operator=(PassEncoding&& other) noexcept
        {
            if (this != &other)
            {
                state_ = std::move(other.state_);
                record_ = std::exchange(other.record_, nullptr);
                policy_ = other.policy_;
            }
            return *this;
        }

        PassEncoding(const PassEncoding&) = delete;
        PassEncoding& operator=(const PassEncoding&) = delete;

        template <PassRecordState State>
        [[nodiscard]] static RenderResult<PassEncoding> create(State state, PassEncodingPolicy policy = {}) noexcept
        {
            if (!policy.valid())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            PassEncoding result;
            result.state_ = {new State(std::move(state)), [](void* p) noexcept { delete static_cast<State*>(p); }};
            result.record_ =
                [](const void* p, const PassRecordInput& input, nri::CommandBuffer& cb, const nri::CoreInterface& core
                ) noexcept { return static_cast<const State*>(p)->record(input, cb, core); };
            result.policy_ = policy;
            return result;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return record_ != nullptr;
        }

        [[nodiscard]] PassEncodingPolicy policy() const noexcept
        {
            return policy_;
        }

        // The common executor owns rendering scope/pipeline/descriptors for MANAGED bodies.
        // EXCLUSIVE encodings own their scope; invalidate on both success and failure.
        template <class BindingCache>
            requires requires(BindingCache& cache) {
                { cache.invalidate() } noexcept;
            }
        [[nodiscard]] RenderResult<void>
        record(
            const PassRecordInput& input,
            nri::CommandBuffer& cb,
            const nri::CoreInterface& core,
            BindingCache& cache
        ) const noexcept
        {
            if (!valid())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto result = record_(state_.get(), input, cb, core);
            if (policy_.invalidatesBindings())
            {
                cache.invalidate();
            }
            return result;
        }

    private:
        std::unique_ptr<void, Delete> state_{nullptr, [](void*) noexcept {}};
        Record record_ = nullptr;
        PassEncodingPolicy policy_;
    };

    template <class Feature, class Config>
    [[nodiscard]] RenderResult<PassEncoding>
    compilePass(const Config& config, PassCompileContext& compiler, PassEncodingPolicy policy = {}) noexcept
    {
        static_assert(noexcept(Feature::compile(config, compiler)));
        auto state = Feature::compile(config, compiler);
        if (!state)
        {
            return cxx::unexpected(state.error());
        }
        return PassEncoding::create(std::move(*state), policy);
    }
} // namespace lux::render
