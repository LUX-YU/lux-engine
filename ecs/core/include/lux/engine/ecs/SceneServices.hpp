#pragma once
// ============================================================================
//  SceneServices.hpp — scene-scoped typed services used during assembly.
//
//  Ownership is explicit:
//    - emplace() transfers unique ownership to this container;
//    - adopt() records a non-null borrow whose owner must outlive the scene.
//
//  This is an assembly-time container, not a hot-path service locator.  It is
//  intentionally move-only in spirit (and currently immovable) so addresses
//  captured by installed systems remain stable.  Owned services are destroyed
//  in reverse registration order because later services may borrow earlier
//  ones.
//
//  Type identity uses lux::cxx::type_hash/type_name.  No RTTI or
//  exception-based error reporting is required: callers receive an expected
//  result and must handle duplicate or null registration explicitly.
// ============================================================================

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/TypeToken.hpp>
#include <lux/engine/ecs/visibility.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    class InstalledSceneServiceBatch;
    class SceneServiceLease;
    class SceneServiceMutationBatch;
    class SceneServiceTransaction;
    class ScheduleBuilder;

    /// Stable, RTTI-free identity used by scene-plan compilation. The type
    /// name is retained as a collision guard for the 64-bit hash.
    using SceneServiceType = TypeToken;

    template <class T>
    [[nodiscard]] constexpr SceneServiceType sceneServiceType() noexcept
    {
        return typeToken<T>();
    }

    [[nodiscard]] constexpr bool sameSceneServiceType(
        SceneServiceType lhs, SceneServiceType rhs) noexcept
    {
        return sameTypeToken(lhs, rhs);
    }

    enum class ESceneServiceRegistrationError : std::uint8_t
    {
        NullService = 0,
        DuplicateType = 1,
        MutationUnavailable = 2,
    };

    [[nodiscard]] constexpr std::string_view toString(
        ESceneServiceRegistrationError error) noexcept
    {
        switch (error)
        {
        case ESceneServiceRegistrationError::NullService:
            return "null_service";
        case ESceneServiceRegistrationError::DuplicateType:
            return "duplicate_type";
        case ESceneServiceRegistrationError::MutationUnavailable:
            return "mutation_unavailable";
        }
        return "unknown";
    }

    template <class T>
    using SceneServiceResult = lux::cxx::expected<T, ESceneServiceRegistrationError>;

    template <class T>
    using SceneServiceRegistration = SceneServiceResult<T*>;

    namespace detail
    {
        struct LUX_ECS_PUBLIC SceneServiceOwner
        {
            virtual ~SceneServiceOwner() noexcept;
        };

        template <class T>
        struct TypedSceneServiceOwner final : SceneServiceOwner
        {
            explicit TypedSceneServiceOwner(std::unique_ptr<T> service) noexcept
                : service_(std::move(service))
            {
            }

            [[nodiscard]] T* get() noexcept { return service_.get(); }

        private:
            std::unique_ptr<T> service_;
        };

        /// Shared generation control for both fixed-lifetime assembly services
        /// and dynamically installed contribution services. The pointee and
        /// all mutations remain owner-thread confined; shared ownership only
        /// keeps stale references inspectable after their service is retired.
        struct SceneServiceState final
        {
            SceneServiceType type{};
            void* ptr{nullptr};
            std::unique_ptr<SceneServiceOwner> owner;
            std::uint32_t generation{1u};
            bool active{false};
        };
    }

    template <class T>
    class SceneServiceRef final
    {
    public:
        SceneServiceRef() noexcept = default;

        [[nodiscard]] T* get() const noexcept
        {
            const auto state = state_.lock();
            if (!state || !state->active ||
                state->generation != generation_ || !state->ptr)
                return nullptr;
            return static_cast<T*>(state->ptr);
        }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return get() != nullptr;
        }

    private:
        friend class SceneServices;
        SceneServiceRef(
            std::weak_ptr<detail::SceneServiceState> state,
            std::uint32_t generation) noexcept
            : state_(std::move(state)), generation_(generation)
        {
        }

        std::weak_ptr<detail::SceneServiceState> state_;
        std::uint32_t generation_{0u};
    };

    /// Move-only lifetime token for one dynamically published service. Reset
    /// invalidates every outstanding SceneServiceRef before destroying the
    /// service. It deliberately carries no raw SceneServices pointer.
    class LUX_ECS_PUBLIC SceneServiceLease final
    {
    public:
        SceneServiceLease() noexcept = default;
        ~SceneServiceLease() noexcept;
        SceneServiceLease(const SceneServiceLease&) = delete;
        SceneServiceLease& operator=(const SceneServiceLease&) = delete;
        SceneServiceLease(SceneServiceLease&& other) noexcept;
        SceneServiceLease& operator=(SceneServiceLease&& other) noexcept;

        void reset() noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class SceneServices;
        friend class SceneServiceTransaction;
        explicit SceneServiceLease(
            std::shared_ptr<detail::SceneServiceState> state) noexcept;

        std::shared_ptr<detail::SceneServiceState> state_;
        std::uint32_t generation_{0u};
    };

    /// Unpublished owner-thread batch used by dynamic scene contributions.
    /// Construction may fail; installing a validated batch is an ownership
    /// move with no domain-specific callbacks or type switches.
    class LUX_ECS_PUBLIC SceneServiceMutationBatch final
    {
    public:
        SceneServiceMutationBatch() = default;
        SceneServiceMutationBatch(const SceneServiceMutationBatch&) = delete;
        SceneServiceMutationBatch& operator=(
            const SceneServiceMutationBatch&) = delete;
        SceneServiceMutationBatch(SceneServiceMutationBatch&&) noexcept =
            default;
        SceneServiceMutationBatch& operator=(
            SceneServiceMutationBatch&&) noexcept = default;

        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> add(
            std::unique_ptr<T> service)
        {
            if (!service)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::NullService);
            const auto type = sceneServiceType<T>();
            if (contains(type))
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);

            auto owner =
                std::make_unique<detail::TypedSceneServiceOwner<T>>(
                    std::move(service));
            T* const registered = owner->get();
            auto state = std::make_shared<detail::SceneServiceState>();
            state->type = type;
            state->ptr = registered;
            state->owner = std::move(owner);
            states_.push_back(std::move(state));
            return registered;
        }

        /// Stage a non-owning service whose owner is installed by the same
        /// outer transaction (for example an ISystem in ScheduleMutationBatch).
        /// The outer owner must retire before the returned service lease.
        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> addBorrowed(T& service)
        {
            const auto type = sceneServiceType<T>();
            if (contains(type))
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);

            auto state = std::make_shared<detail::SceneServiceState>();
            state->type = type;
            state->ptr = &service;
            states_.push_back(std::move(state));
            return &service;
        }

        template <class T, class... Args>
        [[nodiscard]] SceneServiceRegistration<T> add(Args&&... args)
        {
            return add(std::make_unique<T>(std::forward<Args>(args)...));
        }

        template <class T>
        [[nodiscard]] T* get() noexcept
        {
            const auto type = sceneServiceType<T>();
            for (auto it = states_.rbegin(); it != states_.rend(); ++it)
                if (sameSceneServiceType((*it)->type, type))
                    return static_cast<T*>((*it)->ptr);
            return nullptr;
        }

        [[nodiscard]] bool contains(SceneServiceType type) const noexcept
        {
            for (const auto& state : states_)
                if (sameSceneServiceType(state->type, type))
                    return true;
            return false;
        }

        [[nodiscard]] bool empty() const noexcept { return states_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept
        {
            return states_.size();
        }

    private:
        friend class SceneServices;
        std::vector<std::shared_ptr<detail::SceneServiceState>> states_;
    };

    class LUX_ECS_PUBLIC InstalledSceneServiceBatch final
    {
    public:
        InstalledSceneServiceBatch() = default;
        ~InstalledSceneServiceBatch() noexcept;
        InstalledSceneServiceBatch(const InstalledSceneServiceBatch&) = delete;
        InstalledSceneServiceBatch& operator=(
            const InstalledSceneServiceBatch&) = delete;
        InstalledSceneServiceBatch(
            InstalledSceneServiceBatch&&) noexcept = default;
        InstalledSceneServiceBatch& operator=(
            InstalledSceneServiceBatch&&) noexcept = default;

        void reset() noexcept;
        [[nodiscard]] bool valid() const noexcept
        {
            for (const auto& lease : leases_)
                if (lease)
                    return true;
            return false;
        }

    private:
        friend class SceneServices;
        friend class SceneServiceTransaction;
        std::vector<SceneServiceLease> leases_;
    };

    class LUX_ECS_PUBLIC SceneServices
    {
    private:
        enum class EState : std::uint8_t
        {
            Open,
            MutationBlocked,
            Destroying,
        };

        class OperationGuard final
        {
        public:
            OperationGuard(SceneServices& services, EState entered) noexcept
                : services_(services), previous_(services.state_)
            {
                // Destruction is terminal and must never be weakened by a
                // nested unpublished assembly tearing itself down.
                if (previous_ != EState::Destroying)
                {
                    services_.state_ = entered;
                    entered_ = true;
                }
            }

            ~OperationGuard() noexcept
            {
                if (entered_)
                    services_.state_ = previous_;
            }

            OperationGuard(const OperationGuard&) = delete;
            OperationGuard& operator=(const OperationGuard&) = delete;

        private:
            SceneServices& services_;
            EState         previous_;
            bool           entered_{false};
        };

    public:
        SceneServices() = default;
        SceneServices(const SceneServices&)            = delete;
        SceneServices& operator=(const SceneServices&) = delete;
        SceneServices(SceneServices&&)                 = delete;
        SceneServices& operator=(SceneServices&&)      = delete;

        /// The service of type T, or nullptr when absent. O(n) over a small
        /// assembly-time table; systems retain the typed pointer they receive.
        template <class T>
        [[nodiscard]] T* get() noexcept
        {
            if (state_ == EState::Destroying)
                return nullptr;
            const SceneServiceType key = sceneServiceType<T>();
            for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot)
                if ((*slot)->active &&
                    sameSceneServiceType((*slot)->type, key))
                    return static_cast<T*>((*slot)->ptr);
            return nullptr;
        }

        template <class T>
        [[nodiscard]] const T* get() const noexcept
        {
            if (state_ == EState::Destroying)
                return nullptr;
            const SceneServiceType key = sceneServiceType<T>();
            for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot)
                if ((*slot)->active &&
                    sameSceneServiceType((*slot)->type, key))
                    return static_cast<const T*>((*slot)->ptr);
            return nullptr;
        }

        template <class T>
        [[nodiscard]] SceneServiceRef<T> find() const noexcept
        {
            if (state_ == EState::Destroying)
                return {};
            const auto type = sceneServiceType<T>();
            for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot)
            {
                if ((*slot)->active &&
                    sameSceneServiceType((*slot)->type, type))
                {
                    return SceneServiceRef<T>{*slot, (*slot)->generation};
                }
            }
            return {};
        }

        template <class T>
        [[nodiscard]] bool owns() const noexcept
        {
            if (state_ == EState::Destroying)
                return false;
            const SceneServiceType key = sceneServiceType<T>();
            for (auto slot = slots_.rbegin(); slot != slots_.rend(); ++slot)
                if ((*slot)->active &&
                    sameSceneServiceType((*slot)->type, key))
                    return static_cast<bool>((*slot)->owner);
            return false;
        }

        /// Runtime form used by contribution validation. This is still a typed
        /// identity (type_hash + collision-guard name), not RTTI.
        [[nodiscard]] bool contains(SceneServiceType type) const noexcept
        {
            if (state_ == EState::Destroying)
                return false;
            for (const auto& slot : slots_)
                if (slot->active && sameSceneServiceType(slot->type, type))
                    return true;
            return false;
        }

        template <class T>
        [[nodiscard]] bool contains() const noexcept
        {
            return contains(sceneServiceType<T>());
        }

        /// Register an owned service. On failure, the supplied unique_ptr is
        /// destroyed normally and the container remains unchanged.
        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> emplace(std::unique_ptr<T> service)
        {
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable);
            if (!service)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::NullService);

            const SceneServiceType key = sceneServiceType<T>();
            if (contains(key))
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);

            auto owner =
                std::make_unique<detail::TypedSceneServiceOwner<T>>(
                    std::move(service));
            T* registered = owner->get();
            auto slot = std::make_shared<detail::SceneServiceState>();
            slot->type = key;
            slot->ptr = registered;
            slot->owner = std::move(owner);
            slot->active = true;
            slots_.push_back(std::move(slot));
            return registered;
        }

        /// Construct and register an owned service directly.
        template <class T, class... Args>
        [[nodiscard]] SceneServiceRegistration<T> emplace(Args&&... args)
        {
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable);
            if (contains<T>())
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);
            return emplace(std::make_unique<T>(std::forward<Args>(args)...));
        }

        /// Register a borrowed service. The reference expresses the non-null
        /// precondition; its owner must outlive this container and its users.
        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> adopt(T& borrowed)
        {
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable);
            const SceneServiceType key = sceneServiceType<T>();
            if (contains(key))
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType);

            auto slot = std::make_shared<detail::SceneServiceState>();
            slot->type = key;
            slot->ptr = &borrowed;
            slot->active = true;
            slots_.push_back(std::move(slot));
            return &borrowed;
        }

        /// Publish a prevalidated dynamic batch. Its returned owner must be
        /// retained for as long as the services are installed.
        [[nodiscard]] SceneServiceResult<InstalledSceneServiceBatch> install(
            SceneServiceMutationBatch&& batch);

        /// Publish one prevalidated mutation batch while retaining separate
        /// lifetime owners for its logical contribution partitions. All
        /// result storage is reserved before the first service becomes live,
        /// so a dependency closure can be installed as one transaction and
        /// later removed contribution-by-contribution.
        [[nodiscard]] SceneServiceResult<std::vector<InstalledSceneServiceBatch>>
        installPartitioned(
            SceneServiceMutationBatch&& batch,
            std::span<const std::size_t> partition_sizes);

        ~SceneServices() noexcept;

    private:
        friend class SceneServiceLease;
        friend class SceneServiceTransaction;
        friend class ScheduleBuilder;

        /// Roll back registrations made by one unpublished assembly attempt.
        /// Systems borrowing those registrations must be destroyed first;
        /// ScheduleBuilder enforces that order.
        void rollbackTo(std::size_t registration_count) noexcept
        {
            while (slots_.size() > registration_count)
            {
                retire(*slots_.back());
                slots_.pop_back();
            }
        }
        static void retire(detail::SceneServiceState& state) noexcept;
        void pruneRetired() noexcept;

        std::vector<std::shared_ptr<detail::SceneServiceState>> slots_;
        EState            state_{EState::Open};
    };

    /// Unpublished overlay used by ScheduleBuilder. Reads see `staged + base`;
    /// registrations only enter `staged` until the builder commits. Therefore a
    /// failed pack install cannot leave a service in the live table, and staged
    /// systems are destroyed before the services they borrow.
    ///
    /// Assembly is owner-thread confined, so this transaction needs neither a
    /// lock nor shared ownership. Type identity remains TypeToken
    /// (lux::cxx::type_hash + type_name); no RTTI participates.
    class LUX_ECS_PUBLIC SceneServiceTransaction final
    {
    public:
        explicit SceneServiceTransaction(SceneServices& base) noexcept
            : base_(base)
        {
        }

        ~SceneServiceTransaction() = default;

        SceneServiceTransaction(const SceneServiceTransaction&) = delete;
        SceneServiceTransaction& operator=(
            const SceneServiceTransaction&) = delete;
        SceneServiceTransaction(SceneServiceTransaction&&) = delete;
        SceneServiceTransaction& operator=(
            SceneServiceTransaction&&) = delete;

        template <class T>
        [[nodiscard]] const T* get() const noexcept
        {
            if (state_ == EState::Publishing ||
                state_ == EState::Discarding)
                return nullptr;
            if (const auto* staged = staged_.get<T>())
                return staged;
            return base_.get<T>();
        }

        /// Mutable borrow retained by an installed runtime consumer. This is
        /// intentionally distinct from get(): ordinary assembly reads are
        /// const, so mutability is visible at the call site.
        template <class T>
        [[nodiscard]] T* borrow() noexcept
        {
            if (state_ == EState::Publishing ||
                state_ == EState::Discarding)
                return nullptr;
            if (auto* staged = staged_.get<T>())
                return staged;
            return base_.get<T>();
        }

        /// Queue a no-fail edit of a service owned by this unpublished overlay.
        /// The callable is not run during pack installation: publish() applies
        /// queued edits only after topology and service-conflict validation are
        /// complete. Rolling an install back merely destroys the callable, so
        /// even an adopted external object remains unchanged on failure.
        ///
        /// The explicit noexcept contract is intentional. Deferred edits form
        /// the final, non-recoverable publication step; work which can fail
        /// belongs in preflight or in an owned candidate constructed earlier.
        /// If T is also an ISystem, the edit must not change prerequisites,
        /// ordering edges, access declarations, render features, or dynamic-
        /// removal capability: ScheduleBuilder freezes those descriptors before
        /// publication. Descriptor-visible configuration belongs in the
        /// system's construction-time value state, not in this runtime seam.
        template <class T, class Edit>
        [[nodiscard]] bool deferStagedEdit(Edit&& edit)
        {
            using EditType = std::remove_cvref_t<Edit>;
            static_assert(
                std::is_nothrow_invocable_v<EditType&, T&>,
                "a deferred service edit must be noexcept"
            );
            static_assert(
                std::is_nothrow_constructible_v<EditType, Edit&&> &&
                    std::is_nothrow_destructible_v<EditType>,
                "a deferred service edit must have no-fail storage semantics"
            );

            if (state_ != EState::Open)
                return false;
            T* const target = staged_.get<T>();
            if (!target)
                return false;

            deferred_edits_.push_back(
                std::make_unique<DeferredEditModel<T, EditType>>(
                    target,
                    std::forward<Edit>(edit)
                )
            );
            return true;
        }

        [[nodiscard]] bool contains(SceneServiceType type) const noexcept
        {
            if (state_ == EState::Publishing ||
                state_ == EState::Discarding)
                return false;
            return staged_.contains(type) || base_.contains(type);
        }

        template <class T>
        [[nodiscard]] bool contains() const noexcept
        {
            return contains(sceneServiceType<T>());
        }

        template <class T>
        [[nodiscard]] bool owns() const noexcept
        {
            if (state_ == EState::Publishing ||
                state_ == EState::Discarding)
                return false;
            if (staged_.contains<T>())
                return staged_.owns<T>();
            return base_.owns<T>();
        }

        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> emplace(
            std::unique_ptr<T> service
        )
        {
            if (!service)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::NullService
                );
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable
                );
            if (contains<T>())
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType
                );
            return staged_.emplace(std::move(service));
        }

        template <class T, class... Args>
        [[nodiscard]] SceneServiceRegistration<T> emplace(Args&&... args)
        {
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable
                );
            if (contains<T>())
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType
                );
            return emplace(
                std::make_unique<T>(std::forward<Args>(args)...)
            );
        }

        template <class T>
        [[nodiscard]] SceneServiceRegistration<T> adopt(T& borrowed)
        {
            if (state_ != EState::Open)
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::MutationUnavailable
                );
            if (contains<T>())
                return lux::cxx::unexpected(
                    ESceneServiceRegistrationError::DuplicateType
                );
            return staged_.adopt(borrowed);
        }

        [[nodiscard]] bool committed() const noexcept
        {
            return state_ == EState::Published;
        }

    private:
        friend class ScheduleBuilder;

        enum class EState : std::uint8_t
        {
            Open,
            Publishing,
            Discarding,
            Published,
        };

        struct Checkpoint
        {
            std::size_t registrations{0};
            std::size_t deferred_edits{0};
        };

        struct LUX_ECS_PUBLIC DeferredEdit
        {
            virtual ~DeferredEdit() noexcept;
            virtual void apply() noexcept = 0;
        };

        template <class T, class Edit>
        struct DeferredEditModel final : DeferredEdit
        {
            template <class U>
            DeferredEditModel(T* target, U&& edit) noexcept
                : target_(target), edit_(std::forward<U>(edit))
            {
            }

            void apply() noexcept override
            {
                std::invoke(edit_, *target_);
            }

        private:
            T*   target_;
            Edit edit_;
        };

        [[nodiscard]] Checkpoint checkpoint() const noexcept
        {
            return Checkpoint{
                staged_.slots_.size(),
                deferred_edits_.size()
            };
        }

        void discardDeferredFrom(Checkpoint checkpoint) noexcept
        {
            if (state_ != EState::Open)
                return;
            state_ = EState::Discarding;
            deferred_edits_.resize(checkpoint.deferred_edits);
        }

        void rollbackRegistrationsTo(Checkpoint checkpoint) noexcept
        {
            if (state_ != EState::Discarding)
                return;
            staged_.rollbackTo(checkpoint.registrations);
            state_ = EState::Open;
        }

        /// Publish only when every staged type is still absent from base.
        /// Owner-thread confinement removes races, but base may have changed
        /// deliberately between staging and ScheduleBuilder::commit().
        [[nodiscard]] lux::cxx::expected<void, SceneServiceType> publish();

        [[nodiscard]] bool canClaimPublished(
            std::size_t first,
            std::size_t last) const noexcept;
        [[nodiscard]] InstalledSceneServiceBatch claimPublished(
            std::size_t first,
            std::size_t last) noexcept;

        SceneServices& base_;
        SceneServices  staged_;
        std::vector<std::shared_ptr<detail::SceneServiceState>> published_;
        std::vector<bool> published_claimed_;
        std::vector<std::unique_ptr<DeferredEdit>> deferred_edits_;
        EState          state_{EState::Open};
    };

} // namespace lux::ecs
