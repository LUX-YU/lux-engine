#pragma once

#include <lux/engine/simulation/script/ScriptBindings.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/ScriptRuntimeAccess.hpp>
#include <lux/cxx/container/SlotMap.hpp>
#include <entt/container/dense_map.hpp>
#include <limits>
#include <utility>

namespace lux::simulation::script::detail
{
    inline constexpr auto kInvalidPreparedMethod = (std::numeric_limits<std::uint32_t>::max)();

    struct ScriptPreparedMethod final
    {
        lux::script::ScriptSymbolId symbol{};
        ScriptBackendPreparedMethod backend;
        bool used_by_binding{};
    };

    struct ScriptMountView final
    {
        ScriptMountId id;
        lux::asset::AssetId asset;
        ScriptInstanceScope scope;
        ScriptInstanceId instance;
        ScriptInstanceId retiring_instance;
        ecs::Entity entity{ecs::NullEntity};
        EScriptMountState state{EScriptMountState::INACTIVE};
        EScriptEndPlayReason pending_end_reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
        std::uint64_t admission_order{};
        bool pending{};
        bool retirement_queued{};
        bool gameplay_lifetime_started{};
        bool reclaimed{true};
    };

    struct ScriptLifecycleCallError final
    {
        EScriptSystemError error{EScriptSystemError::INVOCATION_FAILURE};
        lux::script::ScriptSymbolId symbol{};
        std::int32_t status{};
    };

    struct ScriptEventSourceAccess final
    {
        std::uint32_t endpoint{};
        PreparedResumeType payload;
        ScriptInstanceScope scope;
    };

    class ScriptInstances final
    {
        struct InvocationState;
    public:
        using Result = lux::cxx::expected<void, EScriptSystemError>;
        using LifecycleResult = lux::cxx::expected<void, ScriptLifecycleCallError>;
        ScriptInstances() = default;
        ScriptInstances(const ScriptInstances&) = delete;
        ScriptInstances& operator=(const ScriptInstances&) = delete;
        ScriptInstances(ScriptInstances&&) = delete;
        ScriptInstances& operator=(ScriptInstances&&) = delete;

        class Protection final
        {
        public:
            explicit Protection(ScriptInstances& owner) noexcept;
            Protection(const Protection&) = delete;
            Protection& operator=(const Protection&) = delete;
            ~Protection() noexcept;
        private:
            ScriptInstances& owner_;
        };

        class Invocation final
        {
        public:
            Invocation(const Invocation&) = delete;
            Invocation& operator=(const Invocation&) = delete;
            Invocation(Invocation&& other) noexcept;
            Invocation& operator=(Invocation&&) = delete;
            ~Invocation() noexcept;
            [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
            [[nodiscard]] bool current() const noexcept;
            [[nodiscard]] bool sameIncarnation() const noexcept;
            [[nodiscard]] const ScriptPreparedMethod& method() const noexcept;
            [[nodiscard]] ScriptInstanceId instance() const noexcept { return instance_; }
        private:
            friend class ScriptInstances;
            Invocation() = default;
            Invocation(ScriptInstances& owner, ScriptInstanceId instance,
                const ScriptPreparedMethod* method, const InvocationState& mount) noexcept;
            ScriptInstances* owner_{};
            ScriptInstanceId instance_;
            const ScriptPreparedMethod* method_{};
            const InvocationState* mount_{};
        };

        class BatchTicket final
        {
        public:
            BatchTicket(const BatchTicket&) = delete;
            BatchTicket& operator=(const BatchTicket&) = delete;
            BatchTicket(BatchTicket&& other) noexcept;
            BatchTicket& operator=(BatchTicket&&) = delete;
            ~BatchTicket() noexcept;
            [[nodiscard]] std::span<const ScriptMountPlacement> placements() const noexcept;
        private:
            friend class ScriptInstances;
            explicit BatchTicket(ScriptInstances& owner) noexcept : owner_(&owner) {}
            ScriptInstances* owner_{};
        };

        class Construction final
        {
        public:
            Construction(const Construction&) = delete;
            Construction& operator=(const Construction&) = delete;
            Construction(Construction&& other) noexcept;
            Construction& operator=(Construction&&) = delete;
            ~Construction() noexcept;
            [[nodiscard]] lux::asset::AssetId assetId() const noexcept;
            [[nodiscard]] const lux::script::ScriptArtifact* artifact() const noexcept;
            void adoptArtifact(ResolvedScriptArtifact artifact) noexcept;
            void selectBackend(const ScriptBackendDescriptor& backend) noexcept;
            [[nodiscard]] Result
            selectLifecycle(lux::script::ScriptSymbolId begin, lux::script::ScriptSymbolId end) noexcept;
            [[nodiscard]] Result reserveCapabilities(std::size_t count) noexcept;
            [[nodiscard]] Result addCapability(const PreparedScriptApiCapability& capability) noexcept;
            [[nodiscard]] Result nextEventLayout() noexcept;
            [[nodiscard]] Result reserveEvents(std::size_t count) noexcept;
            void addEvent(PreparedScriptEventAdmission event) noexcept;
            [[nodiscard]] Result allocateIdentity() noexcept;
            [[nodiscard]] EScriptBackendResult createBackend() noexcept;
            [[nodiscard]] std::size_t methodCount() const noexcept;
            [[nodiscard]] const ScriptPreparedMethod& method(std::size_t local) const noexcept;
            [[nodiscard]] bool lifecycleMethod(std::size_t local) const noexcept;
            [[nodiscard]] EScriptBackendResult
            prepareMethod(std::size_t local, const lux::rdesc::ScriptFunction&) noexcept;
            [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
            void commit() noexcept;
        private:
            friend class ScriptInstances;
            Construction(ScriptInstances& owner, std::uint32_t slot) noexcept : owner_(&owner), slot_(slot) {}
            ScriptInstances* owner_{};
            std::uint32_t slot_{};
        };

        class Retirement final
        {
        public:
            Retirement(const Retirement&) = delete;
            Retirement& operator=(const Retirement&) = delete;
            Retirement(Retirement&&) noexcept = default;
            Retirement& operator=(Retirement&&) noexcept = default;
            [[nodiscard]] std::uint32_t slot() const noexcept { return slot_; }
            [[nodiscard]] ScriptInstanceId instance() const noexcept { return instance_; }
        private:
            friend class ScriptInstances;
            Retirement() = default;
            std::uint32_t slot_{};
            ScriptInstanceId instance_;
            std::uint64_t epoch_{};
            EScriptEndPlayReason reason_{};
            EScriptMountState final_state_{};
        };

        [[nodiscard]] Result prepare(const ScriptRuntimeCapacityPlan& capacity, std::size_t instance_capacity,
            ecs::Registry& registry, ScriptHostApi host) noexcept;
        [[nodiscard]] lux::cxx::expected<BatchTicket, EScriptSystemError> reserveBatch(
            std::span<const ScriptRuntimeMount> inputs, const ScriptBindings& bindings, bool initial
        ) noexcept;
        void commitBatch(BatchTicket&& ticket, const ScriptBindings& bindings) noexcept;
        [[nodiscard]] lux::cxx::expected<std::optional<Construction>, EScriptSystemError>
        beginConstruction(std::uint32_t slot) noexcept;
        [[nodiscard]] Invocation invokeAccess(ScriptMethodReference method) noexcept;
        [[nodiscard]] Invocation resumeAccess(ScriptInstanceId instance) noexcept;
        [[nodiscard]] ScriptMountView view(std::uint32_t slot) const noexcept;
        [[nodiscard]] std::size_t capacity() const noexcept { return mounts_.size(); }
        [[nodiscard]] std::size_t identityCapacity() const noexcept { return identities_.capacity(); }
        [[nodiscard]] std::optional<std::uint32_t> findMount(ScriptMountId id) const noexcept;
        [[nodiscard]] bool valid(ScriptInstanceId instance) const noexcept;
        [[nodiscard]] bool active(ScriptInstanceId instance) const noexcept;
        [[nodiscard]] std::size_t protectedCount() const noexcept { return protection_count_; }
        [[nodiscard]] std::size_t activeCount() const noexcept { return active_count_; }
        [[nodiscard]] lux::script::ScriptSymbolId methodSymbol(std::uint32_t slot) const noexcept;
        [[nodiscard]] lux::cxx::expected<ScriptEventSourceAccess, EScriptEventWaitError>
        eventSource(ScriptInstanceId instance, ScriptEventAdmissionHandle handle) const noexcept;

        [[nodiscard]] LifecycleResult beginPlay(std::uint32_t slot) noexcept;
        void activate(std::uint32_t slot) noexcept;
        [[nodiscard]] ScriptInstanceId revoke(std::uint32_t slot) noexcept;
        [[nodiscard]] ScriptInstanceId fault(std::uint32_t slot) noexcept;
        void reject(std::uint32_t slot, EScriptSystemError error) noexcept;
        void recordError(std::uint32_t slot, EScriptSystemError error) noexcept;
        [[nodiscard]] bool queueRetirement(std::uint32_t slot) noexcept;
        [[nodiscard]] Retirement claimRetirement(
            std::uint32_t slot, EScriptEndPlayReason reason, EScriptMountState final_state
        ) noexcept;
        [[nodiscard]] LifecycleResult endPlay(const Retirement& retirement) noexcept;
        void finishRetirement(const Retirement& retirement) noexcept;
        [[nodiscard]] bool ownsAttachment(std::uint32_t slot, ecs::Entity entity) const noexcept;
        [[nodiscard]] Result projectAttachment(std::uint32_t slot) noexcept;
        void removeAttachment(std::uint32_t slot) noexcept;
        [[nodiscard]] std::optional<std::uint32_t> observeAttachment(ecs::Entity entity, bool destroying) noexcept;
        void restorePendingAfterRollback() noexcept;
        void finishShutdown() noexcept;
        [[nodiscard]] std::optional<ScriptMountStatus> query(ScriptMountId id) const noexcept;
        [[nodiscard]] ScriptMountStatusCollection collect(std::span<ScriptMountStatus> output) noexcept;
        void writeStats(ScriptRuntimeStats& output) const noexcept;

    private:
        struct InvocationState final
        {
            // Fixed-capacity, owner-only authority. Keep per-handler reads out of the cold Mount stride.
            std::size_t method_first{};
            std::size_t method_count{};
            ScriptInstanceId instance;
            ScriptInstanceId retiring_instance;
            EScriptMountState state{EScriptMountState::INACTIVE};
        };
        struct Mount final
        {
            InvocationState* invocation{};
            ScriptMountId id;
            lux::asset::AssetId asset;
            bool entity_scope{};
            std::optional<ScriptInstanceScope> pending_scope;
            ScriptMountStatus status;
            bool unconsumed_result{};
            std::uint64_t admission_order{};
            std::uint32_t begin_play_method{kInvalidPreparedMethod};
            std::uint32_t end_play_method{kInvalidPreparedMethod};
            ScriptInstanceScope scope;
            ScriptBehavior behavior;
            std::vector<PreparedScriptApiCapability> capabilities;
            std::vector<PreparedScriptEventAdmission> event_sources;
            std::uint64_t event_layout_epoch{};
            ResolvedScriptArtifact artifact;
            const ScriptBackendDescriptor* backend{};
            ScriptBackendInstance backend_instance;
            ecs::Entity entity{ecs::NullEntity};
            bool active_counted{};
            bool retirement_queued{};
            bool gameplay_lifetime_started{};
            bool cleanup_claimed{};
            bool end_play_claimed{};
            std::uint64_t retirement_epoch{};
            EScriptEndPlayReason pending_end_reason{EScriptEndPlayReason::OBJECT_UNMATERIALIZED};
        };
        struct IdentityTag;
        using IdentityStorage = lux::cxx::SlotMap<std::uint32_t, IdentityTag>;
        using IdentityKey = IdentityStorage::key_type;
        [[nodiscard]] static IdentityKey key(ScriptInstanceId id) noexcept;
        [[nodiscard]] const std::uint32_t* findActiveSlot(ScriptInstanceId instance) const noexcept;
        void markStatus(std::uint32_t slot) noexcept;
        void deactivate(Mount& mount) noexcept;
        void resetMountRuntime(Mount& mount) noexcept;
        void rollbackConstruction(std::uint32_t slot) noexcept;
        void discardReservation() noexcept;
        [[nodiscard]] lux::cxx::expected<std::uint32_t, EScriptSystemError>
        claimMethod(Mount& mount, lux::script::ScriptSymbolId symbol) noexcept;
        [[nodiscard]] int invokeLifecycle(std::uint32_t method, const EScriptEndPlayReason* reason) noexcept;

        ecs::Registry* registry_{};
        ScriptHostApi host_;
        IdentityStorage identities_;
        std::vector<Mount> mounts_;
        std::vector<InvocationState> invocation_states_;
        std::vector<ScriptPreparedMethod> methods_;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> mount_index_;
        entt::dense_map<ecs::Entity, std::uint32_t> entity_associations_;
        std::uint64_t assembly_configuration_slot_visits_{};
        std::vector<std::uint32_t> changes_;
        std::vector<std::uint8_t> changed_;
        std::vector<std::uint64_t> batch_ids_;
        std::vector<ScriptMountPlacement> batch_slots_;
        std::vector<std::uint8_t> reserved_mounts_;
        std::vector<ecs::Entity> batch_entities_;
        std::span<const ScriptRuntimeMount> staged_inputs_;
        std::size_t enabled_capacity_{};
        std::size_t instance_capacity_{};
        std::size_t configured_count_{};
        std::size_t pending_count_{};
        std::size_t active_count_{};
        std::size_t protection_count_{};
        std::uint64_t admission_sequence_{};
        ScriptEventAdmissionScope event_scope_;
        std::uint64_t event_epoch_{};
        bool suppress_attachment_signal_{};
        bool reservation_active_{};
    };

    // Inline access preserves owner-only writes without a cross-TU call per script invocation.
    inline ScriptInstances::IdentityKey ScriptInstances::key(ScriptInstanceId id) noexcept
    {
        return id.valid() ? IdentityKey{id.slot - 1U, id.generation} : IdentityKey::invalid();
    }
    inline ScriptInstances::Protection::Protection(ScriptInstances& owner) noexcept : owner_(owner)
    {
        ++owner_.protection_count_;
    }
    inline ScriptInstances::Protection::~Protection() noexcept { --owner_.protection_count_; }
    inline ScriptInstances::Invocation::Invocation(
        ScriptInstances& owner, ScriptInstanceId instance,
        const ScriptPreparedMethod* method, const InvocationState& mount
    ) noexcept : owner_(&owner), instance_(instance), method_(method), mount_(&mount)
    {
        // Both arrays have their complete backing before publication. This pin covers these readonly
        // borrows through result handling; current()/sameIncarnation() still reload dynamic authority.
        ++owner_->protection_count_;
    }
    inline ScriptInstances::Invocation::Invocation(Invocation&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), instance_(other.instance_), method_(other.method_),
          mount_(other.mount_) {}
    inline ScriptInstances::Invocation::~Invocation() noexcept
    {
        if (owner_ != nullptr)
            --owner_->protection_count_;
    }
    inline bool ScriptInstances::Invocation::current() const noexcept
    {
        if (owner_ == nullptr)
            return false;
        const auto& mount = *mount_;
        return mount.state == EScriptMountState::ACTIVE && mount.instance == instance_;
    }
    inline const ScriptPreparedMethod& ScriptInstances::Invocation::method() const noexcept
    {
        return *method_;
    }
    inline bool ScriptInstances::Invocation::sameIncarnation() const noexcept
    {
        if (owner_ == nullptr)
            return false;
        const auto& mount = *mount_;
        return mount.instance == instance_ || mount.retiring_instance == instance_;
    }
    inline bool ScriptInstances::valid(ScriptInstanceId instance) const noexcept
    {
        return identities_.find(key(instance));
    }
    inline const std::uint32_t* ScriptInstances::findActiveSlot(ScriptInstanceId instance) const noexcept
    {
        const auto* slot = identities_.find(key(instance));
        if (slot == nullptr)
            return nullptr;
        const auto& state = invocation_states_[*slot];
        return state.state == EScriptMountState::ACTIVE && state.instance == instance ? slot : nullptr;
    }
    inline bool ScriptInstances::active(ScriptInstanceId instance) const noexcept
    {
        return findActiveSlot(instance) != nullptr;
    }
    inline ScriptInstances::Invocation
    ScriptInstances::invokeAccess(ScriptMethodReference method) noexcept
    {
        // A binding already carries the fixed configuration slot. Its authoritative full incarnation
        // check also rejects revoked/reused identities; no second identity-directory lookup is needed.
        if (method.mount_slot >= invocation_states_.size() || !method.instance.valid())
            return {};
        const auto& mount = invocation_states_[method.mount_slot];
        if (mount.state != EScriptMountState::ACTIVE || mount.instance != method.instance)
            return {};
        const bool invalid_method = method.method_slot < mount.method_first ||
            method.method_slot >= mount.method_first + mount.method_count;
        if (invalid_method)
            return {};
        return Invocation{*this, method.instance, &methods_[method.method_slot], mount};
    }
    inline ScriptInstances::Invocation ScriptInstances::resumeAccess(ScriptInstanceId instance) noexcept
    {
        const auto* slot = findActiveSlot(instance);
        if (slot == nullptr)
            return {};
        // No allocation or user code between validation and acquiring the protection ticket.
        return Invocation{*this, instance, nullptr, invocation_states_[*slot]};
    }
    inline lux::script::ScriptSymbolId ScriptInstances::methodSymbol(std::uint32_t slot) const noexcept
    {
        return methods_[slot].symbol;
    }
    inline lux::cxx::expected<ScriptEventSourceAccess, EScriptEventWaitError>
    ScriptInstances::eventSource(
        ScriptInstanceId instance, ScriptEventAdmissionHandle handle
    ) const noexcept
    {
        const auto* slot = findActiveSlot(instance);
        if (slot == nullptr)
            return lux::cxx::unexpected(EScriptEventWaitError::INVALID_INSTANCE);
        const auto& mount = mounts_[*slot];
        const auto local = ScriptRuntimeAccess::matchAdmission(handle, event_scope_, instance,
            mount.event_layout_epoch, mount.event_sources.size());
        if (!local)
            return lux::cxx::unexpected(EScriptEventWaitError::UNDECLARED_SOURCE);
        const auto& source = mount.event_sources[*local];
        return ScriptEventSourceAccess{source.endpoint_slot, source.payload, mount.scope};
    }

}
