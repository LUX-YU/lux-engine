#pragma once

#include <concepts>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/graph/PassRecordInput.hpp>
#include <lux/cxx/compile_time/type_info.hpp>
#include <memory>
#include <optional>
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
    struct GraphResource;

    // Compile-duration format facts. A State must copy any data it retains.
    struct PassRenderingInfo final
    {
        std::span<const nri::Format> colors;
        nri::Format depth = nri::Format::UNKNOWN;
        std::uint8_t samples = 1;
        bool dynamic_depth_bias = false;
    };

    struct PassCompileContext final
    {
        detail::DeviceSession& device;
        const PassInterface& interface;
        // Target-selected artifacts; the selected pass program is relocated to bindings below.
        // These are compile-duration borrows. A returned State must own anything retained.
        std::span<const rdesc::ShaderProgramArtifact> programs;
        PassDescriptorBindings bindings;
        std::optional<PassRenderingInfo> rendering;
        std::span<const GraphResource> resources;
        // Borrowed from the sole device cache for this Plan lifetime. A State must not destroy it.
        nri::Pipeline* pipeline = nullptr;
        std::uint32_t* indexed_list_count = nullptr;
        std::uint32_t* raster_list_count = nullptr;
        std::span<const std::uint8_t> materialized_resources;
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
        std::size_t scratch_bytes = 0;

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
        using RecordResult = RenderResult<void>;
        using Record = RecordResult (*)(
            const void*, const PassRecordInput&, nri::CommandBuffer&, const nri::CoreInterface&
        ) noexcept;
        static constexpr std::size_t inline_bytes_ = 768;

        template <class State>
        static constexpr bool is_inline_ = sizeof(State) <= inline_bytes_ && alignof(State) <= alignof(std::max_align_t);

    public:
        PassEncoding() noexcept = default;
        ~PassEncoding() noexcept { reset(); }

        PassEncoding(PassEncoding&& other) noexcept { moveFrom(other); }

        PassEncoding& operator=(PassEncoding&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                moveFrom(other);
            }
            return *this;
        }

        PassEncoding(const PassEncoding&) = delete;
        PassEncoding& operator=(const PassEncoding&) = delete;

        template <PassRecordState State>
        [[nodiscard]] static RenderResult<PassEncoding> create(State state, PassEncodingPolicy policy = {}) noexcept
        {
            if (!policy.valid()) return cxx::unexpected(RenderError{kDeviceArgument});
            PassEncoding result;
            if constexpr (is_inline_<State>)
            {
                result.state_ = std::construct_at(reinterpret_cast<State*>(result.storage_), std::move(state));
                result.move_ = [](void* from, void* to) noexcept
                {
                    std::construct_at(static_cast<State*>(to), std::move(*static_cast<State*>(from)));
                    std::destroy_at(static_cast<State*>(from));
                };
                result.destroy_ = [](void* p) noexcept { std::destroy_at(static_cast<State*>(p)); };
            }
            else
            {
                result.state_ = new State(std::move(state));
                result.destroy_ = [](void* p) noexcept { delete static_cast<State*>(p); };
            }
            result.record_ =
                [](const void* p, const PassRecordInput& input, nri::CommandBuffer& cb, const nri::CoreInterface& core
                ) noexcept { return static_cast<const State*>(p)->record(input, cb, core); };
            result.type_ = cxx::make_basic_type_info<State>();
            result.policy_ = policy;
            return result;
        }

        template <class State> [[nodiscard]] const State* getIf() const noexcept
        {
            return type_ == cxx::make_basic_type_info<State>() ? static_cast<const State*>(state_) : nullptr;
        }

        template <class State> [[nodiscard]] State* getIf() noexcept
        {
            return type_ == cxx::make_basic_type_info<State>() ? static_cast<State*>(state_) : nullptr;
        }

        template <class State> [[nodiscard]] const State& get() const noexcept
        {
            const auto* value = getIf<State>();
            if (!value) std::terminate();
            return *value;
        }

        template <class State> [[nodiscard]] State& get() noexcept
        {
            auto* value = getIf<State>();
            if (!value) std::terminate();
            return *value;
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
            if (policy_.scope == EPassEncodingScope::EXCLUSIVE) cache.invalidate();
            auto result = record_(state_, input, cb, core);
            if (policy_.invalidatesBindings())
            {
                cache.invalidate();
            }
            return result;
        }

    private:
        void reset() noexcept
        {
            if (state_) destroy_(state_);
            state_ = nullptr;
            record_ = nullptr;
        }

        void moveFrom(PassEncoding& other) noexcept
        {
            record_ = std::exchange(other.record_, nullptr);
            destroy_ = other.destroy_;
            move_ = other.move_;
            type_ = other.type_;
            policy_ = other.policy_;
            if (other.state_ && move_)
            {
                state_ = storage_;
                move_(other.state_, state_);
                other.state_ = nullptr;
            }
            else state_ = std::exchange(other.state_, nullptr);
        }

        alignas(std::max_align_t) std::byte storage_[inline_bytes_];
        void* state_ = nullptr;
        void (*destroy_)(void*) noexcept = nullptr;
        void (*move_)(void*, void*) noexcept = nullptr;
        cxx::basic_type_info type_;
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
