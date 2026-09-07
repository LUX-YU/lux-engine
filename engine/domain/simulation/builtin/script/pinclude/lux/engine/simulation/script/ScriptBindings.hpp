#pragma once

#include <lux/engine/simulation/ScriptSystem.hpp>
#include <lux/engine/simulation/detail/DenseEntityHandlerStorage.hpp>
#include <lux/cxx/container/SlotMap.hpp>

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

    struct ScriptMethodReference final
    {
        std::uint32_t mount_slot{};
        std::uint32_t method_slot{};
        ScriptInstanceId instance;
    };

    // The endpoint ABI borrows this operation port, never the runtime State or its containers.
    struct ScriptBindingDispatch final
    {
        void* context{};
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
        [[nodiscard]] Result connect() noexcept;
        [[nodiscard]] Result disconnect() noexcept;
        template <class Invoke>
        void visitHook(std::uint32_t bucket, Invoke&& invoke) noexcept
        {
            Traversal traversal{*this};
            for (const auto& handler : hooks_[bucket].handlers.values())
                if (configurations_[handler.mount_slot].published)
                    invoke(handler);
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
        struct HookBucket final
        {
            ScriptBindings* owner{};
            std::uint32_t slot{};
            EndpointConnectionToken token;
            HandlerStorage handlers;
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
