#pragma once
/**
 * @file EcsCommandBuffer.hpp
 * @brief 节点私有的单生产者结构命令分片。
 *
 * ── 为什么需要延迟命令(这条没变)────────────────────────────────────────
 *
 * EnTT 的信号在 `emplace` / `erase` 的**过程中**派发:观察者跑的时候,发信号的那个
 * pool 正处在一次修改的中途。给别的 pool 加组件通常可行,但销毁实体、或碰同一个
 * pool 就不安全。所有成熟 ECS 都配了同样的东西(Bevy 的 Commands、Unity 的
 * EntityCommandBuffer、Flecs 在 observer 内自动 defer)。
 *
 * ── 为什么不再是 `std::function<void(Registry&)>` ────────────────────────
 *
 * 闭包把三样东西一起擦掉了:**谁**发的命令、命令是**什么**、以及生产者**还活着吗**。
 * 第三样最要命 —— 旧实现里每个观察者都得自己拿 `weak_ptr` 兜底,于是系统被迫
 * `enable_shared_from_this`,只为了让队列里的闭包能安全地发现自己的宿主没了。
 *
 * 这里换成:命令是**值**,生产者用 `slot + generation` 认。生产者被摘掉时槽位代次
 * 自增,barrier 一比对就知道该丢弃并计数 —— 命令**不会去访问已死的 owner**,
 * `weak_ptr` 那套兜底整个消失。
 *
 * ── 命令长什么样 ────────────────────────────────────────────────────────
 *
 *     struct AddViewRequested
 *     {
 *         using Producer = CameraViewSubsystem;  // 谁能发它
 *         entt::entity entity;                   // 纯值载荷
 *         std::size_t registryPublicationBytes() const noexcept;
 *         void prepareRegistryPublication(EntityRegistry&) const noexcept;
 *         void apply(lux::ecs::Registry&, CameraViewSubsystem&) const;
 *     };
 *
 * `apply` 拿到的是**已经过代次校验**的生产者引用。类型身份在 `push` 时校验一次
 * (`sameSystemType`,hash + 编译期类型名,不只比 hash),之后的下转是安全的 ——
 * 全程无 RTTI、无异常。
 *
 * ⚠️ **命令必须平凡可复制**。载荷放在一段会随写入增长的连续 arena 里,vector 扩容
 * 走的是 memcpy;非平凡类型在那一刻就被撕坏了。这条约束同时挡住了「往命令里塞
 * `shared_ptr`/lease/闭包」的冲动 —— 那类东西本来就该由 owner 持有,不该在命令里
 * 旅行。真出现需要非平凡载荷的命令时再谈,不预留没有消费者的 destroy 槽。
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/RegistryStorageCapacity.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/ecs/visibility.h>
#include <lux/engine/ecs/Registry.hpp>   // lux::ecs::Registry

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::ecs
{
    /// Type-erased lifetime anchor for deferred ECS command producers.
    ///
    /// This deliberately carries no scheduler surface. Both top-level
    /// `ISystem` nodes and RenderSystem-internal `IRenderSubsystem` nodes can
    /// produce structural commands without pretending to be the same kind of
    /// scheduler node. Concrete identity remains `TypeToken`; no RTTI is used.
    class IEcsCommandProducer
    {
    public:
        virtual ~IEcsCommandProducer() = default;

    protected:
        IEcsCommandProducer() = default;
    };

    enum class ECommandEnqueueError : std::uint8_t
    {
        /// writer 没绑到任何槽位(默认构造,或系统已被摘除)。
        NoProducer,
        /// 命令声明的 `Producer` 不是持有这个 writer 的系统。
        ProducerTypeMismatch,
        /// registry-owned publication block could not be armed.
        PublicationReservationFailed,
        /// The fixed owner-time command/payload arena is full.
        CapacityExceeded,
    };

    [[nodiscard]] constexpr std::string_view toString(
        ECommandEnqueueError error) noexcept
    {
        switch (error)
        {
        case ECommandEnqueueError::NoProducer:           return "no_producer";
        case ECommandEnqueueError::ProducerTypeMismatch: return "producer_type_mismatch";
        case ECommandEnqueueError::PublicationReservationFailed:
            return "publication_reservation_failed";
        case ECommandEnqueueError::CapacityExceeded:
            return "capacity_exceeded";
        }
        return "unknown";
    }

    using CommandEnqueueResult = lux::cxx::expected<void, ECommandEnqueueError>;

    /// Conservative bytes for this many possible sparse-page allocation
    /// events across the storages touched by one command. Command types use
    /// this in registryPublicationBytes(); a zero return is an explicit claim
    /// that apply performs no allocating registry mutation or owns a nested
    /// transaction (EntitySection publication is the latter).
    [[nodiscard]] inline std::size_t ecsCommandSparsePublicationBytes(
        std::size_t sparse_events) noexcept
    {
        const auto result = registrySparsePublicationBytes(sparse_events);
        if (!result)
            std::abort();
        return *result;
    }

    /// One barrier-wide accumulator for packed/payload storage admissions.
    /// Claims use the storage's live size as their base and accumulate every
    /// command which will publish in the barrier.  Resetting the plan between
    /// barriers therefore reuses stable high-water capacity instead of
    /// growing it once per historical command.
    class EcsCommandStorageReservationPlan final
    {
    public:
        static constexpr std::size_t kMaximumClaimedStorages = 256u;

        EcsCommandStorageReservationPlan()
        {
            claims_.reserve(kMaximumClaimedStorages);
        }

        EcsCommandStorageReservationPlan(
            const EcsCommandStorageReservationPlan&) = delete;
        EcsCommandStorageReservationPlan& operator=(
            const EcsCommandStorageReservationPlan&) = delete;

        void reset() noexcept
        {
            claims_.clear();
        }

        template <class Storage>
        void reserve(Storage& storage, std::size_t additional) noexcept
        {
            const auto identity = static_cast<const void*>(&storage);
            auto found = claims_.begin();
            for (; found != claims_.end(); ++found)
            {
                if (found->identity == identity)
                    break;
            }
            if (found == claims_.end())
            {
                if (claims_.size() >= kMaximumClaimedStorages ||
                    claims_.size() >= claims_.capacity())
                {
                    std::abort();
                }
                claims_.push_back(Claim{identity, storage.size(), 0u});
                found = claims_.end() - 1;
            }

            std::size_t pending = 0u;
            std::size_t requested = 0u;
            if (!checkedAdditionalCapacity(
                    found->pending, additional, pending) ||
                !checkedAdditionalCapacity(
                    found->base_size, pending, requested) ||
                !reserveStorageCapacity(storage, requested))
            {
                std::abort();
            }
            found->pending = pending;
        }

    private:
        struct Claim final
        {
            const void* identity{nullptr};
            std::size_t base_size{0u};
            std::size_t pending{0u};
        };

        std::vector<Claim> claims_;
    };

    namespace detail
    {
        /// Process-module-independent owner-thread slot for command preflight.
        ///
        /// Command prepare thunks are instantiated in their owning DLL/EXE,
        /// while Schedule lives in ecs_core.  A header-local `thread_local`
        /// therefore gives each PE module a different slot on Windows.  Keep
        /// the slot behind the ecs_core export so every thunk observes the
        /// scope installed by Schedule::prepareCommandBarrier().
        [[nodiscard]] LUX_ECS_PUBLIC
        EcsCommandStorageReservationPlan*&
        activeCommandStorageReservationPlan() noexcept;

    } // namespace detail

    class EcsCommandStorageReservationScope final
    {
    public:
        explicit EcsCommandStorageReservationScope(
            EcsCommandStorageReservationPlan& plan) noexcept
            : plan_(&plan)
        {
            auto*& active =
                detail::activeCommandStorageReservationPlan();
            if (active)
                std::abort();
            plan_->reset();
            active = plan_;
        }

        ~EcsCommandStorageReservationScope() noexcept
        {
            auto*& active =
                detail::activeCommandStorageReservationPlan();
            if (active != plan_)
                std::abort();
            active = nullptr;
        }

        EcsCommandStorageReservationScope(
            const EcsCommandStorageReservationScope&) = delete;
        EcsCommandStorageReservationScope& operator=(
            const EcsCommandStorageReservationScope&) = delete;

    private:
        EcsCommandStorageReservationPlan* plan_{nullptr};
    };

    /// Adds one typed storage claim to the current owner-time barrier
    /// preflight. Calling this outside prepareRegistryPublication() is an
    /// invariant failure.
    template <class Storage>
    void reserveEcsCommandStorage(
        Storage& storage,
        std::size_t additional) noexcept
    {
        auto* plan = detail::activeCommandStorageReservationPlan();
        if (!plan)
            std::abort();
        plan->reserve(storage, additional);
    }

    /// 一个节点的命令分片。单生产者:只有拥有它的那个系统(以及它装的观察者)写。
    /// 归 `Schedule` 所有,与槽位同生共死。
    class EcsCommandBuffer final
    {
    public:
        /// 载荷已经过代次校验,`producer` 保证是命令声明的那个具体类型。
        using ApplyFn = void (*)(
            lux::ecs::Registry&,
            IEcsCommandProducer&,
            const void*
        );
        using PrepareFn = void (*)(
            lux::ecs::Registry&,
            const void*
        ) noexcept;

        struct Header final
        {
            ApplyFn       apply{nullptr};
            PrepareFn     prepare{nullptr};
            /// 生产者槽位的代次快照。barrier 比对当前代次,不等即丢弃并计数 ——
            /// 这就是「命令不访问死亡 owner」的全部机制。
            std::uint32_t producer_generation{0};
            /// 分片内的稳定序号。合并顺序 = 编译后的节点序 × 本序号。
            std::uint32_t local_sequence{0};
            std::uint32_t payload_offset{0};
            std::size_t publication_bytes{0u};
            bool publication_prepared{false};
            lux::ecs::RegistryPublicationReservation
                publication_reservation;

            Header() = default;
            Header(
                ApplyFn apply_value,
                PrepareFn prepare_value,
                std::uint32_t generation_value,
                std::uint32_t sequence_value,
                std::uint32_t offset_value,
                std::size_t publication_bytes_value,
                lux::ecs::RegistryPublicationReservation reservation)
                noexcept
                : apply(apply_value),
                  prepare(prepare_value),
                  producer_generation(generation_value),
                  local_sequence(sequence_value),
                  payload_offset(offset_value),
                  publication_bytes(publication_bytes_value),
                  publication_prepared(false),
                  publication_reservation(std::move(reservation))
            {}

            Header(const Header&) = delete;
            Header& operator=(const Header&) = delete;
            Header(Header&&) noexcept = default;
            Header& operator=(Header&&) noexcept = default;
        };

        static constexpr std::size_t kMaximumCommands = 2048u;
        static constexpr std::size_t kMaximumPayloadBytes = 128u * 1024u;

        EcsCommandBuffer()
        {
            headers_.reserve(kMaximumCommands);
            payload_.reserve(kMaximumPayloadBytes);
        }

        EcsCommandBuffer(const EcsCommandBuffer&)            = delete;
        EcsCommandBuffer& operator=(const EcsCommandBuffer&) = delete;
        EcsCommandBuffer(EcsCommandBuffer&&)                 = default;
        EcsCommandBuffer& operator=(EcsCommandBuffer&&)      = default;

        template <class Cmd>
        [[nodiscard]] CommandEnqueueResult emplace(
            const Cmd& cmd,
            std::uint32_t producer_generation,
            lux::ecs::Registry& registry)
        {
            static_assert(std::is_trivially_copyable_v<Cmd>,
                "ECS 命令必须平凡可复制:载荷住在会 memcpy 扩容的 arena 里"
                "(见本文件头)。");
            static_assert(alignof(Cmd) <= alignof(std::max_align_t),
                "过对齐的命令载荷:arena 只保证 max_align_t 对齐。");
            static_assert(requires(const Cmd& value)
                {
                    { value.registryPublicationBytes() } ->
                        std::same_as<std::size_t>;
                },
                "ECS command must declare registryPublicationBytes() so "
                "the barrier can arm a no-grow publication transaction.");
            static_assert(noexcept(cmd.registryPublicationBytes()),
                "registryPublicationBytes() must be noexcept.");
            static_assert(requires(
                const Cmd& value,
                lux::ecs::Registry& target)
                {
                    { value.prepareRegistryPublication(target) } ->
                        std::same_as<void>;
                },
                "ECS command must declare "
                "prepareRegistryPublication(EntityRegistry&).");
            static_assert(noexcept(
                cmd.prepareRegistryPublication(registry)),
                "prepareRegistryPublication() must be noexcept.");

            const auto publication_bytes =
                cmd.registryPublicationBytes();
            lux::ecs::RegistryPublicationReservation reservation;
            if (publication_bytes != 0u)
            {
                auto armed = registry.reservePublication(publication_bytes);
                if (armed)
                {
                    reservation = std::move(*armed);
                }
                else if (armed.error() !=
                    lux::ecs::ERegistryPublicationReservationError::
                        PUBLICATION_ACTIVE)
                {
                    return lux::cxx::unexpected(
                        ECommandEnqueueError::
                            PublicationReservationFailed);
                }
                // An active publication or barrier-wide admission gate means
                // this is a re-entrant command for the next barrier. It stays
                // in the fixed shard and receives its private reservation in
                // the next preflight below.
            }

            if (payload_.size() >
                static_cast<std::size_t>(-1) - (alignof(Cmd) - 1u))
            {
                return lux::cxx::unexpected(
                    ECommandEnqueueError::CapacityExceeded);
            }
            const std::size_t offset = alignUp(payload_.size(), alignof(Cmd));
            if (headers_.size() >= kMaximumCommands ||
                offset > kMaximumPayloadBytes ||
                sizeof(Cmd) > kMaximumPayloadBytes - offset ||
                headers_.size() >= headers_.capacity() ||
                offset > payload_.capacity() ||
                sizeof(Cmd) > payload_.capacity() - offset)
            {
                return lux::cxx::unexpected(
                    ECommandEnqueueError::CapacityExceeded);
            }
            payload_.resize(offset + sizeof(Cmd));
            std::memcpy(payload_.data() + offset, &cmd, sizeof(Cmd));

            headers_.emplace_back(
                &applyThunk<Cmd>,
                &prepareThunk<Cmd>,
                producer_generation,
                next_sequence_++,
                static_cast<std::uint32_t>(offset),
                publication_bytes,
                std::move(reservation));
            return {};
        }

        [[nodiscard]] bool empty() const noexcept { return headers_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return headers_.size(); }

        [[nodiscard]] std::span<Header> headers() noexcept
        {
            return headers_;
        }

        [[nodiscard]] std::span<const Header> headers() const noexcept
        {
            return headers_;
        }

        [[nodiscard]] bool armReservations(
            lux::ecs::Registry& registry) noexcept
        {
            for (auto& header : headers_)
            {
                if (header.publication_bytes == 0u)
                {
                    continue;
                }
                if (!header.publication_reservation)
                {
                    auto armed = registry.reservePublication(
                        header.publication_bytes);
                    if (!armed)
                        return false;
                    header.publication_reservation = std::move(*armed);
                }
                header.prepare(registry, payloadOf(header));
                header.publication_prepared = true;
            }
            return true;
        }

        [[nodiscard]] bool reservationsReady() const noexcept
        {
            for (const auto& header : headers_)
            {
                if (header.publication_bytes != 0u &&
                    (!header.publication_reservation ||
                     !header.publication_prepared))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] const void* payloadOf(const Header& h) const noexcept
        {
            return payload_.data() + h.payload_offset;
        }

        /// 换出待应用的内容,留一个空分片继续接收 —— barrier 期新入队的命令
        /// 因此落在**下一轮**。序号不重置:同一个生产者的命令序在整个场景生命期
        /// 单调,trace 才对得上。
        void takeInto(EcsCommandBuffer& out) noexcept
        {
            out.headers_.swap(headers_);
            out.payload_.swap(payload_);
            headers_.clear();
            payload_.clear();
        }

        void clear() noexcept
        {
            headers_.clear();
            payload_.clear();
        }

        /// Owner-time override. Values above the hard maxima are rejected so
        /// no caller can accidentally turn a command barrier into a vector
        /// growth point.
        void reserve(std::size_t commands, std::size_t payload_bytes)
        {
            if (commands > kMaximumCommands ||
                payload_bytes > kMaximumPayloadBytes)
            {
                std::abort();
            }
            headers_.reserve(commands);
            payload_.reserve(payload_bytes);
        }

    private:
        [[nodiscard]] static constexpr std::size_t alignUp(
            std::size_t value, std::size_t alignment) noexcept
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        template <class Cmd>
        static void prepareThunk(
            lux::ecs::Registry& registry,
            const void* payload) noexcept
        {
            static_cast<const Cmd*>(payload)->prepareRegistryPublication(
                registry);
        }

        template <class Cmd>
        static void applyThunk(lux::ecs::Registry& registry,
                               IEcsCommandProducer& producer,
                               const void* payload)
        {
            using Producer = typename Cmd::Producer;
            static_assert(std::is_base_of_v<IEcsCommandProducer, Producer>,
                "命令的 Producer 必须是一个 ECS command producer。");
            // 下转在这里是安全的:类型身份已在 push 时对着槽位校验过,
            // 代次已在 barrier 校验过。无 RTTI。
            static_cast<const Cmd*>(payload)->apply(
                registry, static_cast<Producer&>(producer));
        }

        std::vector<Header>    headers_;
        std::vector<std::byte> payload_;
        std::uint32_t          next_sequence_{0};
    };

    /// 往某个节点的分片里写的凭据。**可复制的小 POD** —— 系统在 `onAdded` 收下它、
    /// 存进自己的状态,装的观察者就能在任意时刻入队,不必等到 `update` 才拿到。
    ///
    /// 它不是所有权:分片归 `Schedule`。系统被摘除后槽位代次自增,写进去的命令会在
    /// barrier 被判掉 —— 所以持有一个过期 writer 是**安全的**,只是没有效果。
    class EcsCommandWriter final
    {
    public:
        EcsCommandWriter() = default;

        [[nodiscard]] bool valid() const noexcept { return buffer_ != nullptr; }

        template <class Cmd>
        [[nodiscard]] CommandEnqueueResult push(const Cmd& cmd) const
        {
            if (!buffer_)
                return lux::cxx::unexpected<ECommandEnqueueError>(
                    ECommandEnqueueError::NoProducer);
            // 直接使用基础层 TypeToken，避免通过 ISystem.hpp 形成 include 环。
            if (producer_type_ !=
                lux::cxx::typeToken<typename Cmd::Producer>())
                return lux::cxx::unexpected<ECommandEnqueueError>(
                    ECommandEnqueueError::ProducerTypeMismatch);

            if (!registry_)
                return lux::cxx::unexpected<ECommandEnqueueError>(
                    ECommandEnqueueError::NoProducer);
            return buffer_->emplace(cmd, generation_, *registry_);
        }

    private:
        friend class Schedule;
        friend class EcsCommandOwner;

        EcsCommandWriter(
            EcsCommandBuffer& buffer,
            lux::cxx::TypeToken producer_type,
            std::uint32_t generation,
            lux::ecs::Registry& registry) noexcept
            : buffer_(&buffer), producer_type_(producer_type),
              generation_(generation), registry_(&registry)
        {
        }

        EcsCommandBuffer* buffer_{nullptr};
        lux::cxx::TypeToken producer_type_{};
        std::uint32_t     generation_{0};
        lux::ecs::Registry* registry_{nullptr};
    };

    /// Owns one producer's stable command shard and its generation.
    ///
    /// The buffer is indirect so moving a compiled plan cannot invalidate a
    /// writer captured by an EnTT observer. `retire()` invalidates every
    /// outstanding writer before the concrete producer is destroyed.
    class EcsCommandOwner final
    {
    public:
        explicit EcsCommandOwner(lux::cxx::TypeToken producer_type)
            : buffer_(std::make_unique<EcsCommandBuffer>()),
              producer_type_(producer_type)
        {
        }

        EcsCommandOwner(const EcsCommandOwner&) = delete;
        EcsCommandOwner& operator=(const EcsCommandOwner&) = delete;
        EcsCommandOwner(EcsCommandOwner&&) noexcept = default;
        EcsCommandOwner& operator=(EcsCommandOwner&&) noexcept = default;

        [[nodiscard]] EcsCommandWriter writer(
            lux::ecs::Registry& registry) noexcept
        {
            if (!buffer_)
                return {};
            return EcsCommandWriter{
                *buffer_,
                producer_type_,
                generation_,
                registry,
            };
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return !buffer_ || buffer_->empty();
        }

        void takeInto(EcsCommandBuffer& staging) noexcept
        {
            if (buffer_)
                buffer_->takeInto(staging);
        }

        void retire() noexcept
        {
            ++generation_;
            if (generation_ == 0)
                ++generation_;
        }

        [[nodiscard]] std::uint32_t generation() const noexcept
        {
            return generation_;
        }

    private:
        std::unique_ptr<EcsCommandBuffer> buffer_;
        lux::cxx::TypeToken               producer_type_{};
        std::uint32_t                     generation_{1};
    };

} // namespace lux::ecs
