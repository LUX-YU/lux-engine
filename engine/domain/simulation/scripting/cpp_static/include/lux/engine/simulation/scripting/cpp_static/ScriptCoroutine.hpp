#pragma once

#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/detail/BoundedFrameStorage.hpp>
#include <lux/engine/simulation/scripting/cpp_static/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lux::script
{
    template <class Ability, class Context>
    class ScriptAbilityCoroutine;
}

namespace lux::simulation::script
{
    struct DelayAbility;

    namespace detail
    {
        struct ScriptCoroutineAbilityAccess final
        {
            void* context{};
            const void* dispatch{};
        };
    }

    enum class EScriptCoroutineError : std::uint8_t
    {
        INVALID_CONTEXT,
        UNDECLARED_ABILITY,
        CAPACITY_EXCEEDED,
        FRAME_ALLOCATION_FAILURE,
        RESULT_MISMATCH,
    };

    template <class Payload>
    class CppScriptEventSource final
    {
    public:
        static_assert(lux::semantic::TypeDeclared<Payload>);
        static_assert(std::is_trivially_copyable_v<Payload>);

        [[nodiscard]] static lux::cxx::expected<CppScriptEventSource, EScriptCoroutineError> create(
            lux::script::ScriptEventSourceDescription description
        ) noexcept
        {
            using Traits = lux::semantic::TypeTraits<Payload>;
            const bool is_mismatch = !description.valid() ||
                description.payload.type_id != lux::semantic::typeId(Traits::CanonicalName) ||
                description.payload.canonical_name != Traits::CanonicalName ||
                description.payload.abi_kind != Traits::AbiKind || description.payload.size != Traits::Size ||
                description.payload.alignment != Traits::Alignment;
            return is_mismatch
                ? lux::cxx::unexpected<EScriptCoroutineError>(EScriptCoroutineError::RESULT_MISMATCH)
                : lux::cxx::expected<CppScriptEventSource, EScriptCoroutineError>{
                    CppScriptEventSource(std::move(description))
                };
        }

        [[nodiscard]] const lux::script::ScriptEventSourceDescription& description() const noexcept
        {
            return description_;
        }

    private:
        explicit CppScriptEventSource(lux::script::ScriptEventSourceDescription description) noexcept
            : description_(std::move(description))
        {
        }

        lux::script::ScriptEventSourceDescription description_;
    };

    class ScriptCoroutine;
    struct ScriptCoroutinePromiseAccess;

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC ScriptCoroutineContext final
    {
    public:
        ScriptCoroutineContext() noexcept = default;

        template <class Payload>
        [[nodiscard]] auto wait(const CppScriptEventSource<Payload>& source) noexcept;

        template <class Ability>
        [[nodiscard]] lux::cxx::expected<
            lux::script::ScriptAbilityCoroutine<Ability, ScriptCoroutineContext>,
            EScriptCoroutineError
        > ability() noexcept;

        [[nodiscard]] lux::script::ScriptAbilityCoroutine<DelayAbility, ScriptCoroutineContext> delay() noexcept;

    private:
        using FindAbilityFn = bool (*)(void*, std::uint32_t, std::uint64_t, std::uint32_t&) noexcept;
        using ResolveAbilityFn = bool (*)(
            void*,
            std::uint32_t,
            std::uint32_t,
            detail::ScriptCoroutineAbilityAccess&
        ) noexcept;

        template <class Result, class Invoker>
        [[nodiscard]] auto invokeAbility(std::uint32_t ability_slot, Invoker&& invoker) noexcept;

        template <class Result, class Starter>
        [[nodiscard]] auto awaitAbility(std::uint32_t ability_slot, Starter starter) noexcept;

        template <class Result, class Admission>
        [[nodiscard]] auto makeAwaiter(Admission admission) noexcept;

        [[nodiscard]] bool resolveAbility(
            std::uint32_t ability_slot,
            detail::ScriptCoroutineAbilityAccess& result
        ) const noexcept
        {
            return active_step_ != nullptr && resolve_ability_ != nullptr &&
                resolve_ability_(backend_, instance_slot_, ability_slot, result);
        }

        struct FrameHeader final
        {
            detail::BoundedFrameStorage* storage{};
            detail::BoundedFrameStorage::Allocation allocation;
        };

        ScriptCoroutineContext(
            void* backend,
            std::uint32_t instance_slot,
            FindAbilityFn find_ability,
            ResolveAbilityFn resolve_ability,
            detail::BoundedFrameStorage& frame_storage
        ) noexcept
            : backend_(backend),
              instance_slot_(instance_slot),
              find_ability_(find_ability),
              resolve_ability_(resolve_ability),
              frame_storage_(std::addressof(frame_storage))
        {
        }

        [[nodiscard]] std::optional<std::uint32_t> findAbility(std::uint64_t contract_hash) const noexcept
        {
            std::uint32_t result{};
            return find_ability_ != nullptr && find_ability_(backend_, instance_slot_, contract_hash, result)
                ? std::optional<std::uint32_t>{result}
                : std::nullopt;
        }

        [[nodiscard]] void* allocateFrame(std::size_t size, std::size_t alignment) noexcept
        {
            if (frame_storage_ == nullptr || size == 0U || alignment == 0U ||
                (alignment & (alignment - 1U)) != 0U)
            {
                return nullptr;
            }
            const auto padding = sizeof(FrameHeader) + alignment - 1U;
            if (size > (std::numeric_limits<std::size_t>::max)() - padding)
                return nullptr;
            auto allocation = frame_storage_->acquire(size + padding, alignment);
            if (!allocation)
                return nullptr;
            const auto raw = reinterpret_cast<std::uintptr_t>(allocation->data) + sizeof(FrameHeader);
            const auto aligned = (raw + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U);
            auto* header = reinterpret_cast<FrameHeader*>(aligned - sizeof(FrameHeader));
            std::construct_at(header, FrameHeader{frame_storage_, *allocation});
            return reinterpret_cast<void*>(aligned);
        }

        static void releaseFrame(void* frame) noexcept
        {
            if (frame == nullptr)
                return;
            auto* header = reinterpret_cast<FrameHeader*>(
                static_cast<std::byte*>(frame) - sizeof(FrameHeader)
            );
            if (header->storage != nullptr)
                static_cast<void>(header->storage->release(header->allocation));
            std::destroy_at(header);
        }

        void activate(ScriptStepContext& step, const ScriptResumePacket* packet) noexcept
        {
            active_step_ = std::addressof(step);
            resume_packet_ = packet;
        }

        void deactivate() noexcept
        {
            active_step_ = nullptr;
            resume_packet_ = nullptr;
        }

        void* backend_{};
        std::uint32_t instance_slot_{};
        FindAbilityFn find_ability_{};
        ResolveAbilityFn resolve_ability_{};
        detail::BoundedFrameStorage* frame_storage_{};
        ScriptStepContext* active_step_{};
        const ScriptResumePacket* resume_packet_{};

        friend class ScriptCoroutine;
        friend struct ScriptCoroutinePromiseAccess;
        friend struct CppStaticCoroutineAccess;
        template <class Ability, class Context>
        friend class lux::script::ScriptAbilityCoroutine;
        template <class Result, class Admission>
        friend class ScriptCoroutineAwaiter;
    };

    struct ScriptCoroutinePromiseAccess final
    {
        [[nodiscard]] static void* allocate(
            ScriptCoroutineContext& context,
            std::size_t size,
            std::size_t alignment
        ) noexcept
        {
            return context.allocateFrame(size, alignment);
        }

        static void release(void* frame) noexcept
        {
            ScriptCoroutineContext::releaseFrame(frame);
        }
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC ScriptCoroutine final
    {
    public:
        struct promise_type final
        {
            [[nodiscard]] ScriptCoroutine get_return_object() noexcept
            {
                return ScriptCoroutine{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            [[nodiscard]] static ScriptCoroutine get_return_object_on_allocation_failure() noexcept
            {
                return {};
            }

            [[nodiscard]] std::suspend_never initial_suspend() const noexcept
            {
                return {};
            }

            [[nodiscard]] std::suspend_always final_suspend() const noexcept
            {
                return {};
            }

            void return_void() noexcept
            {
                if (outcome.state != EScriptStepState::FAILED)
                    outcome = ScriptStepResult::completed();
            }

            [[noreturn]] void unhandled_exception() noexcept
            {
                std::terminate();
            }

            template <class... Arguments>
            static void* operator new(
                std::size_t size,
                ScriptCoroutineContext& context,
                Arguments&&...
            ) noexcept
            {
                return ScriptCoroutinePromiseAccess::allocate(context, size, alignof(std::max_align_t));
            }

            template <class Owner, class... Arguments>
            static void* operator new(
                std::size_t size,
                Owner&,
                ScriptCoroutineContext& context,
                Arguments&&...
            ) noexcept
            {
                return ScriptCoroutinePromiseAccess::allocate(context, size, alignof(std::max_align_t));
            }

            static void operator delete(void* frame, std::size_t) noexcept
            {
                ScriptCoroutinePromiseAccess::release(frame);
            }

            void suspend(ScriptStepResult result, lux::semantic::TypeId type, std::size_t size) noexcept
            {
                outcome = result;
                expected_type = type;
                expected_size = size;
            }

            [[nodiscard]] bool prepareResume(const ScriptResumePacket& packet) noexcept
            {
                if (packet.state != EScriptAwaitableState::READY)
                {
                    outcome = ScriptStepResult::failed(packet.error.valid() ? packet.error.status : -1);
                    return false;
                }
                const bool expects_value = expected_type != lux::semantic::InvalidTypeId;
                const bool has_value = packet.value != nullptr && packet.value->type.has_value();
                const bool is_value_mismatch = expects_value != has_value ||
                    (expects_value && (packet.value->type->type_id != expected_type ||
                        packet.value->bytes.size() != expected_size));
                if (is_value_mismatch)
                {
                    outcome = ScriptStepResult::failed(-1);
                    return false;
                }
                resume_packet = std::addressof(packet);
                outcome = ScriptStepResult::completed();
                return true;
            }

            void clearResume() noexcept
            {
                resume_packet = nullptr;
            }

            template <class Result>
            [[nodiscard]] Result result() const noexcept
            {
                static_assert(std::is_trivially_copyable_v<Result>);
                Result value{};
                if (resume_packet != nullptr && resume_packet->value != nullptr)
                    std::memcpy(std::addressof(value), resume_packet->value->bytes.data(), sizeof(Result));
                return value;
            }

            ScriptStepResult outcome;
            lux::semantic::TypeId expected_type{lux::semantic::InvalidTypeId};
            std::size_t expected_size{};
            const ScriptResumePacket* resume_packet{};
        };

        ScriptCoroutine() noexcept = default;
        ScriptCoroutine(const ScriptCoroutine&) = delete;
        ScriptCoroutine& operator=(const ScriptCoroutine&) = delete;

        ScriptCoroutine(ScriptCoroutine&& other) noexcept
            : handle_(std::exchange(other.handle_, {}))
        {
        }

        ScriptCoroutine& operator=(ScriptCoroutine&& other) noexcept
        {
            if (this == std::addressof(other))
                return *this;
            reset();
            handle_ = std::exchange(other.handle_, {});
            return *this;
        }

        ~ScriptCoroutine()
        {
            reset();
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return static_cast<bool>(handle_);
        }

    private:
        explicit ScriptCoroutine(std::coroutine_handle<promise_type> handle) noexcept
            : handle_(handle)
        {
        }

        void reset() noexcept
        {
            if (handle_)
                handle_.destroy();
            handle_ = {};
        }

        [[nodiscard]] std::coroutine_handle<promise_type> release() noexcept
        {
            return std::exchange(handle_, {});
        }

        std::coroutine_handle<promise_type> handle_{};
        friend struct CppStaticCoroutineAccess;
    };

    template <class Result, class Admission>
    class ScriptCoroutineAwaiter final
    {
    public:
        ScriptCoroutineAwaiter(ScriptCoroutineContext& context, Admission admission) noexcept
            : context_(std::addressof(context)), admission_(std::move(admission))
        {
        }

        [[nodiscard]] constexpr bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<ScriptCoroutine::promise_type> coroutine) noexcept
        {
            promise_ = std::addressof(coroutine.promise());
            if (context_ == nullptr || context_->active_step_ == nullptr)
            {
                promise_->suspend(ScriptStepResult::failed(-1), lux::semantic::InvalidTypeId, 0U);
                return;
            }
            const auto result = admission_(*context_, *context_->active_step_);
            if constexpr (std::is_void_v<Result>)
            {
                promise_->suspend(result, lux::semantic::InvalidTypeId, 0U);
            }
            else
            {
                promise_->suspend(
                    result,
                    lux::semantic::typeId(lux::semantic::TypeTraits<Result>::CanonicalName),
                    sizeof(Result)
                );
            }
        }

        [[nodiscard]] Result await_resume() const noexcept
            requires (!std::is_void_v<Result>)
        {
            return promise_->template result<Result>();
        }

        void await_resume() const noexcept
            requires std::is_void_v<Result>
        {
        }

    private:
        ScriptCoroutineContext* context_{};
        Admission admission_;
        ScriptCoroutine::promise_type* promise_{};
    };

    template <class Result, class Admission>
    auto ScriptCoroutineContext::makeAwaiter(Admission admission) noexcept
    {
        return ScriptCoroutineAwaiter<Result, Admission>{*this, std::move(admission)};
    }

    template <class Payload>
    auto ScriptCoroutineContext::wait(const CppScriptEventSource<Payload>& source) noexcept
    {
        const auto request = ScriptEventWaitRequest{
            lux::system::SystemInstanceId{source.description().system_id},
            EventPointId{source.description().event_id},
            source.description().route == lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                ? EEventRoute::SIMULATION_BROADCAST
                : EEventRoute::ENTITY_TARGETED
        };
        return makeAwaiter<Payload>([request](ScriptCoroutineContext&, ScriptStepContext& step) noexcept {
            const auto waiting = step.event_waits.wait(request);
            return waiting ? ScriptStepResult::suspended(*waiting) : ScriptStepResult::failed(-1);
        });
    }

    template <class Ability>
    lux::cxx::expected<
        lux::script::ScriptAbilityCoroutine<Ability, ScriptCoroutineContext>,
        EScriptCoroutineError
    > ScriptCoroutineContext::ability() noexcept
    {
        const auto slot = findAbility(lux::script::ScriptAbilityTraits<Ability>::Description.id.hash());
        return slot
            ? lux::cxx::expected<
                lux::script::ScriptAbilityCoroutine<Ability, ScriptCoroutineContext>,
                EScriptCoroutineError
            >{lux::script::ScriptAbilityCoroutine<Ability, ScriptCoroutineContext>{*this, *slot}}
            : lux::cxx::unexpected<EScriptCoroutineError>(EScriptCoroutineError::UNDECLARED_ABILITY);
    }

    template <class Result, class Invoker>
    auto ScriptCoroutineContext::invokeAbility(std::uint32_t ability_slot, Invoker&& invoker) noexcept
    {
        using Value = std::remove_cvref_t<Result>;
        detail::ScriptCoroutineAbilityAccess access;
        if (!resolveAbility(ability_slot, access))
        {
            if constexpr (std::is_void_v<Result>)
                return lux::cxx::expected<void, EScriptCoroutineError>{
                    lux::cxx::unexpected<EScriptCoroutineError>(EScriptCoroutineError::UNDECLARED_ABILITY)
                };
            else
                return lux::cxx::expected<Value, EScriptCoroutineError>{
                    lux::cxx::unexpected<EScriptCoroutineError>(EScriptCoroutineError::UNDECLARED_ABILITY)
                };
        }
        if constexpr (std::is_void_v<Result>)
        {
            std::invoke(std::forward<Invoker>(invoker), access.context, access.dispatch);
            return lux::cxx::expected<void, EScriptCoroutineError>{};
        }
        else
        {
            static_assert(std::is_nothrow_constructible_v<Value, Result>);
            return lux::cxx::expected<Value, EScriptCoroutineError>{Value{
                std::invoke(std::forward<Invoker>(invoker), access.context, access.dispatch)
            }};
        }
    }

    template <class Result, class Starter>
    auto ScriptCoroutineContext::awaitAbility(std::uint32_t ability_slot, Starter starter) noexcept
    {
        return makeAwaiter<Result>(
            [ability_slot, starter = std::move(starter)](
                ScriptCoroutineContext& context,
                ScriptStepContext& step
            ) mutable noexcept
            {
                detail::ScriptCoroutineAbilityAccess access;
                if (!context.resolveAbility(ability_slot, access))
                    return ScriptStepResult::failed(-1);
                return invokeScriptAbilityAsync<Result>(
                    step,
                    [&](lux::script::ScriptAbilityCompletion<Result> completion) noexcept
                    {
                        return starter(access.context, access.dispatch, std::move(completion));
                    }
                );
            }
        );
    }

    struct CppStaticCoroutineAccess final
    {
        template <class Result, class Admission>
        [[nodiscard]] static auto makeAwaiter(
            ScriptCoroutineContext& context,
            Admission admission
        ) noexcept
        {
            return context.template makeAwaiter<Result>(std::move(admission));
        }

        [[nodiscard]] static ScriptCoroutineContext context(
            void* backend,
            std::uint32_t instance_slot,
            ScriptCoroutineContext::FindAbilityFn find_ability,
            ScriptCoroutineContext::ResolveAbilityFn resolve_ability,
            detail::BoundedFrameStorage& frame_storage
        ) noexcept
        {
            return {backend, instance_slot, find_ability, resolve_ability, frame_storage};
        }

        static void activate(
            ScriptCoroutineContext& context,
            ScriptStepContext& step,
            const ScriptResumePacket* packet = nullptr
        ) noexcept
        {
            context.activate(step, packet);
        }

        static void deactivate(ScriptCoroutineContext& context) noexcept
        {
            context.deactivate();
        }

        [[nodiscard]] static std::coroutine_handle<ScriptCoroutine::promise_type> release(
            ScriptCoroutine& coroutine
        ) noexcept
        {
            return coroutine.release();
        }
    };
}
