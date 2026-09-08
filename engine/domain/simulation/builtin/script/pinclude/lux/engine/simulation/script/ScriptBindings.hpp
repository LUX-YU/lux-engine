#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>
#include <lux/cxx/container/SlotMap.hpp>

#include <bit>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lux::simulation::script::detail
{
    struct ScriptMountPlacement final
    {
        std::uint32_t slot{};
        bool existing{};
    };

    struct ScriptBindingLayout final
    {
        std::size_t method_first{};
        std::size_t method_count{};
    };

    class PreparedInvocation;
    class ScriptExecution;

    struct ScriptMethodReference final
    {
        std::uint32_t mount_slot{};
        std::uint32_t method_slot{};
        ScriptInstanceId instance;
        const PreparedInvocation* prepared{};
        void (*entry)(ScriptExecution&, const ScriptMethodReference&, lux_script_call_frame&, bool) noexcept{};
    };

    // The endpoint ABI borrows this operation port, never the runtime State or its containers.
    struct ScriptBindingDispatch final
    {
        void* context{};
        bool (*prepare)(void*, ScriptMethodReference&) noexcept{};
        void (*hook)(void*, std::uint32_t, lux_script_call_frame&) noexcept{};
        void (*event)(void*, std::uint32_t, ecs::Entity, lux_script_call_frame&) noexcept{};
    };

    class ScriptBindings final
    {
    public:
        using Result = lux::cxx::expected<void, EScriptSystemError>;
        ScriptBindings() = default;
        ScriptBindings(const ScriptBindings&) = delete;
        ScriptBindings& operator=(const ScriptBindings&) = delete;
        ScriptBindings(ScriptBindings&&) = delete;
        ScriptBindings& operator=(ScriptBindings&&) = delete;

        class BatchTicket final
        {
        public:
            BatchTicket(const BatchTicket&) = delete;
            BatchTicket& operator=(const BatchTicket&) = delete;
            BatchTicket(BatchTicket&& other) noexcept;
            BatchTicket& operator=(BatchTicket&&) = delete;
            ~BatchTicket() noexcept;

        private:
            friend class ScriptBindings;
            explicit BatchTicket(ScriptBindings& owner) noexcept : owner_(&owner) {}
            ScriptBindings* owner_{};
        };

        [[nodiscard]] Result prepare(
            const SimulationDescription& simulation,
            const ScriptRuntimeCapacityPlan& capacity,
            std::span<const ScriptHookEndpointDescriptor> hooks,
            std::span<const ScriptEventEndpointDescriptor> events,
            ScriptBindingDispatch dispatch,
            std::size_t max_resume_payload
        ) noexcept;

        [[nodiscard]] lux::cxx::expected<BatchTicket, EScriptSystemError> reserveBatch(
            std::span<const ScriptRuntimeMount> inputs,
            std::span<const ScriptMountPlacement> placements
        ) noexcept;
        void commitBatch(BatchTicket&& ticket) noexcept;

        [[nodiscard]] ScriptBindingLayout layout(std::uint32_t slot) const noexcept;
        [[nodiscard]] bool
        matches(std::uint32_t slot, std::span<const ScriptBindingDescription> bindings) const noexcept;
        [[nodiscard]] lux::script::ScriptSymbolId methodSymbol(std::size_t method_slot) const noexcept;
        [[nodiscard]] bool methodUsedByBinding(std::size_t method_slot) const noexcept;
        [[nodiscard]] std::size_t methodCount() const noexcept { return symbols_.size(); }
        [[nodiscard]] std::size_t backingBytes() const noexcept;
        [[nodiscard]] std::uint64_t assemblyEndpointCountVisits() const noexcept
        {
            return assembly_endpoint_count_visits_;
        }
        [[nodiscard]] std::optional<std::uint32_t> findHook(HookScriptTarget target) const noexcept;
        [[nodiscard]] std::optional<std::uint32_t> findEvent(EventScriptTarget target) const noexcept;
        [[nodiscard]] const ScriptEventEndpointDescriptor& eventEndpoint(std::uint32_t slot) const noexcept;
        [[nodiscard]] Result
        validateMethods(std::uint32_t slot, const lux::script::ScriptArtifact& artifact) const noexcept;

        [[nodiscard]] Result publish(std::uint32_t slot, ScriptInstanceId instance, ecs::Entity entity) noexcept;
        void withdraw(std::uint32_t slot) noexcept;
        void setMethodRunnable(std::uint32_t method, ScriptInstanceId instance, bool runnable) noexcept;
        [[nodiscard]] Result connect() noexcept;
        [[nodiscard]] Result disconnect() noexcept;
        template <class Invoke>
        void visitHook(std::uint32_t bucket, Invoke&& invoke) noexcept
        {
            Traversal traversal{*this};
            auto& lane = hooks_[bucket];
            const auto& handlers = lane.handlers.values();
            std::size_t cursor{};
            while (cursor < handlers.size())
            {
                // Re-read after every user call, including nested dispatch. Never revisit a passed position.
                const auto next = lane.runnable.count() == handlers.size() ? cursor : lane.runnable.next(cursor);
                if (next >= handlers.size())
                    break;
                cursor = next + 1U;
                invoke(handlers[next]);
            }
        }
        template <class Invoke>
        void visitEvent(std::uint32_t bucket, ecs::Entity entity, Invoke&& invoke) noexcept
        {
            Traversal traversal{*this};
            const auto visit = [this, &invoke](const ScriptMethodReference& handler) noexcept {
                if (configurations_[handler.mount_slot].published)
                    invoke(handler);
            };
            if (event_endpoints_[bucket].route == EEventRoute::SIMULATION_BROADCAST)
                events_[bucket].handlers.forEachAll(visit);
            else
                events_[bucket].handlers.forEachTarget(entity, visit);
        }

    private:
        enum class EBindingKind : std::uint8_t { HOOK, EVENT };
        struct EndpointKey final
        {
            std::uint64_t system{};
            std::uint64_t endpoint{};
            friend bool operator==(EndpointKey, EndpointKey) noexcept = default;
        };
        struct EndpointKeyHash final
        {
            [[nodiscard]] std::size_t operator()(EndpointKey key) const noexcept;
        };
        struct HandlerTag;
        using HandlerStorage = lux::cxx::SlotMap<ScriptMethodReference, HandlerTag>;
        using HandlerKey = HandlerStorage::key_type;
        using EventHandlerStorage = lux::simulation::detail::DenseEntityHandlerStorage<ScriptMethodReference>;
        class RunnableIndex final
        {
        public:
            void prepare(std::size_t capacity)
            {
                while (capacity != 0U)
                {
                    capacity = (capacity + 63U) / 64U;
                    levels_.emplace_back(capacity, 0U);
                    if (capacity == 1U)
                        break;
                }
            }
            [[nodiscard]] std::size_t count() const noexcept { return count_; }
            [[nodiscard]] bool test(std::size_t index) const noexcept
            {
                return (levels_[0][index / 64U] & (std::uint64_t{1U} << (index % 64U))) != 0U;
            }
            void set(std::size_t index, bool enabled) noexcept
            {
                if (test(index) == enabled)
                    return;
                if (enabled) ++count_; else --count_;
                for (auto& level : levels_)
                {
                    auto& word = level[index / 64U];
                    const auto old = word;
                    const auto mask = std::uint64_t{1U} << (index % 64U);
                    if (enabled) word |= mask; else word &= ~mask;
                    if ((old == 0U) == (word == 0U))
                        break;
                    enabled = word != 0U;
                    index /= 64U;
                }
            }
            [[nodiscard]] std::size_t next(std::size_t index) const noexcept { return nextAt(0U, index); }
            [[nodiscard]] std::size_t backingBytes() const noexcept
            {
                std::size_t bytes{};
                for (const auto& level : levels_) bytes += level.capacity() * sizeof(std::uint64_t);
                return bytes;
            }
        private:
            [[nodiscard]] std::size_t nextAt(std::size_t depth, std::size_t index) const noexcept
            {
                constexpr auto end = (std::numeric_limits<std::size_t>::max)();
                if (depth == levels_.size() || index / 64U >= levels_[depth].size())
                    return end;
                const auto& level = levels_[depth];
                auto word = level[index / 64U] & (~std::uint64_t{} << (index % 64U));
                if (word != 0U)
                    return (index / 64U) * 64U + std::countr_zero(word);
                const auto block = nextAt(depth + 1U, index / 64U + 1U);
                return block == end ? end : block * 64U + std::countr_zero(level[block]);
            }
            std::vector<std::vector<std::uint64_t>> levels_;
            std::size_t count_{};
        };
        struct HookBucket final
        {
            ScriptBindings* owner{};
            std::uint32_t slot{};
            EndpointConnectionToken token;
            HandlerStorage handlers;
            RunnableIndex runnable;
            std::size_t capacity{};
        };
        struct EventBucket final
        {
            ScriptBindings* owner{};
            std::uint32_t slot{};
            EndpointConnectionToken token;
            EventHandlerStorage handlers;
            std::size_t capacity{};
        };
        struct Binding final
        {
            EBindingKind kind{EBindingKind::HOOK};
            std::uint32_t bucket{};
            std::uint32_t method{};
            EndpointConnectionToken registration;
            std::uint32_t next_hook{(std::numeric_limits<std::uint32_t>::max)()};
        };
        struct Configuration final
        {
            std::size_t first{};
            std::size_t count{};
            ScriptBindingLayout methods;
            bool entity_scope{};
            bool published{};
            bool pending_unlink{};
        };
        struct Traversal final
        {
            explicit Traversal(ScriptBindings& owner) noexcept : owner_(owner) { ++owner_.traversal_depth_; }
            ~Traversal() noexcept;
            ScriptBindings& owner_;
        };

        static void hookEntry(void* context, lux_script_call_frame& frame) noexcept;
        static void eventEntry(void* context, ecs::Entity entity, lux_script_call_frame& frame) noexcept;
        void unlink(std::uint32_t slot) noexcept;
        void finishTraversal() noexcept;
        void discardReservation() noexcept;
        [[nodiscard]] Result validateEndpoints(const SimulationDescription& simulation);

        std::vector<ScriptHookEndpointDescriptor> hook_endpoints_;
        std::vector<ScriptEventEndpointDescriptor> event_endpoints_;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> hook_index_;
        std::unordered_map<EndpointKey, std::uint32_t, EndpointKeyHash> event_index_;
        std::vector<HookBucket> hooks_;
        std::vector<EventBucket> events_;
        std::vector<Configuration> configurations_;
        std::vector<Binding> bindings_;
        std::vector<std::uint32_t> method_hooks_;
        std::vector<ScriptBindingDescription> descriptions_;
        std::vector<lux::script::ScriptSymbolId> symbols_;
        std::vector<std::size_t> hook_counts_;
        std::vector<std::size_t> event_counts_;
        std::vector<std::size_t> hook_reservations_;
        std::vector<std::size_t> event_reservations_;
        std::vector<std::uint32_t> pending_unlinks_;
        ScriptBindingDispatch dispatch_;
        std::size_t binding_capacity_{};
        std::size_t method_capacity_{};
        std::size_t max_resume_payload_{};
        std::size_t traversal_depth_{};
        std::span<const ScriptRuntimeMount> staged_inputs_;
        std::span<const ScriptMountPlacement> staged_placements_;
        bool reservation_active_{};
        bool staged_new_configurations_{};
        std::uint64_t assembly_endpoint_count_visits_{};
    };
}
