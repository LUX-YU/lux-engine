#pragma once

#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptSyncStepCall.hpp>
#include <lux/engine/simulation/scripting/cpp_static/visibility.h>
#include <lux/engine/simulation/scripting/detail/BoundedClassStorage.hpp>

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
            PreparedLocalAsyncCatalog local_async;
        };
    } // namespace detail

    enum class EScriptCoroutineError : std::uint8_t
    {
        INVALID_CONTEXT,
        UNDECLARED_ABILITY,
        CAPACITY_EXCEEDED,
        FRAME_ALLOCATION_FAILURE,
        RESULT_MISMATCH,
    };

    struct CppStaticContract;
    namespace detail
    {
        struct CppStaticPreparedEvents final
        {
            const CppStaticContract* layout{};
            std::span<const ScriptEventAdmissionHandle> entries;
        };
        [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
        lux::cxx::expected<std::uint32_t, EScriptCoroutineError> prepareCppEventImport(
            const CppStaticContract& contract, const lux::script::ScriptEventSourceDescription& source) noexcept;
    }

    template <class Payload>
    class CppScriptEventSource final
    {
    public:
        static_assert(lux::semantic::TypeDeclared<Payload>);
        static_assert(std::is_trivially_copyable_v<Payload>);

        [[nodiscard]] static lux::cxx::expected<CppScriptEventSource, EScriptCoroutineError> create(
            const CppStaticContract& contract, const lux::script::ScriptEventSourceDescription& description
        ) noexcept
        {
            using Traits = lux::semantic::TypeTraits<Payload>;
            const bool is_mismatch = !description.valid() ||
                description.payload.type_id != lux::semantic::typeId(Traits::CanonicalName) ||
                description.payload.canonical_name != Traits::CanonicalName ||
                description.payload.abi_kind != Traits::AbiKind || description.payload.size != Traits::Size ||
                description.payload.alignment != Traits::Alignment;
            if (is_mismatch)
                return lux::cxx::unexpected<EScriptCoroutineError>(EScriptCoroutineError::RESULT_MISMATCH);
            const auto slot = detail::prepareCppEventImport(contract, description);
            if (!slot) return lux::cxx::unexpected<EScriptCoroutineError>(slot.error());
            return CppScriptEventSource(&contract, *slot);
        }

    private:
        CppScriptEventSource(const CppStaticContract* layout, std::uint32_t slot) noexcept
            : layout_(layout), local_slot_(slot)
        {
        }

        const CppStaticContract* layout_{};
        std::uint32_t local_slot_{};
        friend class ScriptCoroutineContext;
    };

    class ScriptCoroutineContext;

    // An immutable binding borrowed by one live task. Like its context, this must
    // not escape the owning coroutine's lifetime. It conveys no call permission.
    template <class Signature>
    class ScriptCoroutineSyncStep final
    {
    public:
        template <class... Args> [[nodiscard]] auto operator()(Args&&... args) const noexcept;
    private:
        friend class ScriptCoroutineContext;
        ScriptCoroutineSyncStep(ScriptCoroutineContext& context, const PreparedScriptSyncStep& entry,
            std::uint32_t ordinal) noexcept : context_(&context), entry_(&entry), ordinal_(ordinal) {}
        ScriptCoroutineContext* context_{};
        const PreparedScriptSyncStep* entry_{};
        std::uint32_t ordinal_{};
    };

    class ScriptCoroutine;
    class ScriptCoroutineFailure;
    struct ScriptCoroutinePromiseAccess;

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC ScriptCoroutineContext final
    {
    public:
        ScriptCoroutineContext() noexcept = default;

        template<class Signature, class... Args>
        [[nodiscard]] auto callStep(std::uint32_t ordinal, Args&&... args) noexcept
        {
            return detail::ScriptSyncStepCall<Signature>::invoke(*this, ordinal, std::forward<Args>(args)...);
        }

        template <class Signature>
        [[nodiscard]] lux::cxx::expected<ScriptCoroutineSyncStep<Signature>, ScriptSyncStepError>
        bindStep(std::uint32_t ordinal) noexcept
        {
            const auto entry = resolveSyncStep(ordinal, detail::ScriptSyncStepCall<Signature>::Shape);
            if (!entry) return lux::cxx::unexpected<ScriptSyncStepError>(entry.error());
            return ScriptCoroutineSyncStep<Signature>{*this, **entry, ordinal};
        }

        [[nodiscard]] ScriptCoroutineFailure fail(ScriptSyncStepError error) noexcept;


        template <class Payload>
        [[nodiscard]] auto wait(const CppScriptEventSource<Payload>& source) noexcept;

        template <class Ability>
        [[nodiscard]] lux::cxx::expected<
            lux::script::ScriptAbilityCoroutine<Ability, ScriptCoroutineContext>,
            EScriptCoroutineError
        > ability() noexcept;

        [[nodiscard]] lux::script::ScriptAbilityCoroutine<DelayAbility, ScriptCoroutineContext> delay() noexcept;

    private:
      [[nodiscard]] lux::cxx::expected<void, ScriptSyncStepError> invokeSyncStep(std::uint32_t ordinal,
                                                                                 const ScriptSyncStepShape& shape,
                                                                                 const void* const* arguments,
                                                                                 void* output) noexcept;
      [[nodiscard]] lux::cxx::expected<const PreparedScriptSyncStep*, ScriptSyncStepError>
      resolveSyncStep(std::uint32_t ordinal, const ScriptSyncStepShape& shape) noexcept;
      [[nodiscard]] lux::cxx::expected<void, ScriptSyncStepError>
      invokeResolvedSyncStep(const PreparedScriptSyncStep& entry, std::uint32_t ordinal,
          const void* const* arguments, void* output) noexcept;
      template <class Signature> friend class ScriptCoroutineSyncStep;
      template <class Signature> friend struct detail::ScriptSyncStepCall;

      using FindAbilityFn = bool (*)(void*, std::uint32_t, std::uint64_t, std::uint32_t&) noexcept;
      using ResolveAbilityFn = bool (*)(void*, std::uint32_t, std::uint32_t,
                                        detail::ScriptCoroutineAbilityAccess&) noexcept;

      template <class Result, class Invoker>
      [[nodiscard]] auto invokeAbility(std::uint32_t ability_slot, Invoker&& invoker) noexcept;

      template <class Result, class Starter>
      [[nodiscard]] auto awaitAbility(std::uint32_t ability_slot, Starter starter) noexcept;

      template <class Result, class Arguments, class Starter>
      [[nodiscard]] auto awaitPreparedAbility(std::uint32_t ability_slot,
                                              const lux::script::ScriptApiMethodIdView& method, Arguments arguments,
                                              Starter starter) noexcept;

      template <class Result, class Admission> [[nodiscard]] auto makeAwaiter(Admission admission) noexcept;

      [[nodiscard]] bool resolveAbility(std::uint32_t ability_slot,
                                        detail::ScriptCoroutineAbilityAccess& result) const noexcept
      {
          return active_step_ != nullptr && resolve_ability_ != nullptr &&
                 resolve_ability_(backend_, instance_slot_, ability_slot, result);
      }

        struct FrameHeader final
        {
            detail::BoundedClassStorage::Ticket ticket;
        };

        ScriptCoroutineContext(
            void* backend,
            std::uint32_t instance_slot,
            FindAbilityFn find_ability,
            ResolveAbilityFn resolve_ability,
            detail::BoundedClassStorage& frame_storage,
            detail::BoundedClassStorage::ClassHandle frame_class,
            std::size_t frame_limit,
            std::size_t alignment_limit,
            const detail::CppStaticPreparedEvents* events,
            const ScriptSyncStepSetView* sync_steps
        ) noexcept
            : sync_steps_(sync_steps), sync_publication_(sync_steps ? sync_steps->publication : 0U),
              backend_(backend),
              instance_slot_(instance_slot),
              find_ability_(find_ability),
              resolve_ability_(resolve_ability),
              events_(events),
              frame_storage_(std::addressof(frame_storage)), frame_class_(frame_class),
              frame_limit_(frame_limit), alignment_limit_(alignment_limit)
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
            if (frame_storage_ == nullptr || size == 0U || size > frame_limit_ ||
                alignment > alignment_limit_ || alignment == 0U ||
                (alignment & (alignment - 1U)) != 0U)
            {
                return nullptr;
            }
            alignment = (std::max)(alignment, alignof(FrameHeader));
            const auto padding = sizeof(FrameHeader) + alignment - 1U;
            if (size > (std::numeric_limits<std::size_t>::max)() - padding)
                return nullptr;
            auto allocation = frame_storage_->acquire(frame_class_, size + padding);
            if (!allocation)
                return nullptr;
            const auto raw = reinterpret_cast<std::uintptr_t>(allocation->data) + sizeof(FrameHeader);
            const auto aligned = (raw + alignment - 1U) & ~(static_cast<std::uintptr_t>(alignment) - 1U);
            auto* header = reinterpret_cast<FrameHeader*>(aligned - sizeof(FrameHeader));
            std::construct_at(header, FrameHeader{frame_storage_->ticket(*allocation)});
            return reinterpret_cast<void*>(aligned);
        }

        static void releaseFrame(void* frame) noexcept
        {
            if (frame == nullptr)
                return;
            auto* header = reinterpret_cast<FrameHeader*>(
                static_cast<std::byte*>(frame) - sizeof(FrameHeader)
            );
            const auto ticket = header->ticket;
            std::destroy_at(header);
            if (ticket.owner != nullptr)
                static_cast<void>(ticket.owner->release(ticket));
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

        const ScriptSyncStepSetView* sync_steps_{};
        std::uint64_t sync_publication_{};
        void* backend_{};
        std::uint32_t instance_slot_{};
        FindAbilityFn find_ability_{};
        ResolveAbilityFn resolve_ability_{};
        const detail::CppStaticPreparedEvents* events_{};
        detail::BoundedClassStorage* frame_storage_{};
        detail::BoundedClassStorage::ClassHandle frame_class_;
        std::size_t frame_limit_{};
        std::size_t alignment_limit_{};
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

    template <class Signature>
    template <class... Args>
    auto ScriptCoroutineSyncStep<Signature>::operator()(Args&&... args) const noexcept
    {
        return detail::ScriptSyncStepCall<Signature>::apply(
            [this](const void* const* arguments, void* result) noexcept {
                return context_->invokeResolvedSyncStep(*entry_, ordinal_, arguments, result);
            }, std::forward<Args>(args)...
        );
    }

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
                const bool has_value = packet.value != nullptr && packet.value->type.valid();
                const bool is_value_mismatch = expects_value != has_value ||
                    (expects_value && (packet.value->type.type_id != expected_type ||
                        packet.value->bytes.size() != expected_size));
                if (is_value_mismatch)
                {
                    outcome = ScriptStepResult::failed(-1);
                    return false;
                }
                resume_data = expects_value ? packet.value->bytes.data() : nullptr;
                outcome = ScriptStepResult::completed();
                return true;
            }

            void clearResume() noexcept { resume_data = nullptr; }

          private:
            template <class Result>
            [[nodiscard]] Result result() const noexcept
            {
                static_assert(std::is_trivially_copyable_v<Result>);
                // Only the suspended typed awaiter reads this, after prepareResume
                // validated state/type/size. The packet owner outlives this synchronous
                // resume window.
                Result value;
                std::memcpy(std::addressof(value), resume_data, sizeof(Result));
                return value;
            }

            template <class Result, class Admission> friend class ScriptCoroutineAwaiter;

          public:
            ScriptStepResult outcome;
            lux::semantic::TypeId expected_type{lux::semantic::InvalidTypeId};
            std::size_t expected_size{};

          private:
            const void* resume_data{};
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
                constexpr auto result_type = lux::semantic::typeId(lux::semantic::TypeTraits<Result>::CanonicalName);
                promise_->suspend(result, result_type, sizeof(Result));
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
        return makeAwaiter<Payload>([layout = source.layout_, slot = source.local_slot_](
            ScriptCoroutineContext& context, ScriptStepContext& step) noexcept {
            const auto* events = context.events_;
            const bool wrong_source = events == nullptr || events->layout != layout || slot >= events->entries.size();
            if (wrong_source) return ScriptStepResult::failed(-1);
            const auto waiting = step.event_waits.wait(events->entries[slot]);
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

    template<class Result, class Arguments, class Starter>
    auto ScriptCoroutineContext::awaitPreparedAbility(
        std::uint32_t slot, const lux::script::ScriptApiMethodIdView& method, Arguments arguments, Starter starter
    ) noexcept
    {
        // Generated projection supplies a static immutable descriptor; no per-frame
        // copy of its name/hash.
        return makeAwaiter<Result>([
            slot, method = &method, arguments = std::move(arguments), starter = std::move(starter)
        ](
            ScriptCoroutineContext& context, ScriptStepContext& step) mutable noexcept {
            detail::ScriptCoroutineAbilityAccess access;
            if (!context.resolveAbility(slot, access)) return ScriptStepResult::failed(-1);
            const auto local = access.local_async.resolve(*method, access.context, access.dispatch);
            return std::apply([&](auto&... values) noexcept {
                return invokePreparedScriptAbilityAsync<Result>(step, local,
                    [&](lux::script::ScriptAbilityCompletion<Result> completion) noexcept {
                        return starter(access.context, access.dispatch, std::move(completion), values...);
                    }, values...);
            }, arguments);
        });
    }

    class ScriptCoroutineFailure final
    {
    public:
        explicit ScriptCoroutineFailure(std::int32_t status) noexcept : status_(status != 0 ? status : -1) {}
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<ScriptCoroutine::promise_type> handle) const noexcept
        {
            handle.promise().suspend(ScriptStepResult::failed(status_), lux::semantic::InvalidTypeId, 0U);
        }
        [[noreturn]] void await_resume() const noexcept { std::terminate(); }
    private:
        std::int32_t status_;
    };

    inline ScriptCoroutineFailure ScriptCoroutineContext::fail(ScriptSyncStepError error) noexcept
    {
        return ScriptCoroutineFailure(error.status);
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
            detail::BoundedClassStorage& frame_storage,
            detail::BoundedClassStorage::ClassHandle frame_class,
            std::size_t frame_limit,
            std::size_t alignment_limit,
            const detail::CppStaticPreparedEvents* events,
            const ScriptSyncStepSetView* sync_steps = nullptr
        ) noexcept
        {
            return {backend, instance_slot, find_ability, resolve_ability, frame_storage,
                frame_class, frame_limit, alignment_limit, events, sync_steps};
        }

        [[nodiscard]] static constexpr std::size_t frameOverhead(std::size_t alignment) noexcept
        {
            return sizeof(ScriptCoroutineContext::FrameHeader) +
                (std::max)(alignment, alignof(ScriptCoroutineContext::FrameHeader)) - 1U;
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
} // namespace lux::simulation::script
