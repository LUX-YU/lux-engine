#pragma once

#include <lux/engine/resource/deployment/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::deployment
{
    struct CapacityDomainIdTag final {};
    using CapacityDomainIdView = lux::cxx::StableNameIdView<CapacityDomainIdTag>;
    using CapacityDomainId = lux::cxx::StableNameId<CapacityDomainIdTag>;

    [[nodiscard]] constexpr CapacityDomainIdView capacityDomainId(
        std::string_view name) noexcept
    {
        return CapacityDomainIdView{name};
    }

    inline constexpr auto kActiveRenderInstancesCapacity =
        capacityDomainId("lux.render.instances");
    inline constexpr auto kClassicMeshRecordsCapacity =
        capacityDomainId("lux.render.classic_mesh.records");
    inline constexpr auto kClassicMeshGeometryBytesCapacity =
        capacityDomainId("lux.render.classic_mesh.geometry_bytes");

    enum class ECapacityRequestMode : std::uint8_t
    {
        AUTO,
        EXPLICIT
    };

    enum class ECapacityPlanReason : std::uint8_t
    {
        AUTO_DEFAULT,
        REQUESTED,
        DEVICE_CLAMP,
        PROTOCOL_CLAMP,
        BUDGET_REJECT
    };

    enum class ECapacityCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_DOMAIN,
        ID_COLLISION,
    };

    enum class ECapacityPlanError : std::uint8_t
    {
        INVALID_REQUEST,
        DUPLICATE_REQUEST,
        UNKNOWN_DOMAIN,
        DEVICE_LIMIT,
        PROTOCOL_LIMIT,
        BUDGET_LIMIT,
    };

    struct RuntimeCapacityValue final
    {
        ECapacityRequestMode mode{ECapacityRequestMode::AUTO};
        std::uint64_t value{0u};

        [[nodiscard]] static constexpr RuntimeCapacityValue automatic()
            noexcept
        {
            return {};
        }

        [[nodiscard]] static constexpr RuntimeCapacityValue exact(
            std::uint64_t requested) noexcept
        {
            return {ECapacityRequestMode::EXPLICIT, requested};
        }
    };

    struct RuntimeCapacityRequestEntry final
    {
        CapacityDomainId domain;
        RuntimeCapacityValue value{};
    };

    /// Recipe-owned requests. Absence means AUTO. Programmatic composition may
    /// use set(); disk decoders must reject duplicate keys before calling it.
    struct RuntimeCapacityRequest final
    {
        std::vector<RuntimeCapacityRequestEntry> domains;

        LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC void set(
            CapacityDomainIdView domain,
            RuntimeCapacityValue value);
        [[nodiscard]] LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC
        const RuntimeCapacityValue* find(
            CapacityDomainIdView domain) const noexcept;
    };

    /// Raw device facts. Domain-specific limits are contributed separately to
    /// CapacityDomainCatalog before the immutable plan is resolved.
    struct RuntimeDeviceCapabilities final
    {
        std::uint64_t vram_budget_bytes{0u};
        std::uint64_t vram_usage_bytes{0u};
        std::uint64_t max_storage_buffer_range{0u};
        bool buffer_device_address{false};
        bool shader_int64{false};
    };

    /// One cold-path capacity domain. Values are logical units; byte admission
    /// rounds them up to units_per_granule and charges bytes_per_granule.
    struct CapacityDomainDescriptor final
    {
        CapacityDomainId id;
        std::uint64_t device_limit{0u};
        std::uint64_t protocol_limit{0u};
        std::uint64_t automatic_target{0u};
        std::uint64_t minimum{1u};
        std::uint64_t units_per_granule{1u};
        std::uint64_t bytes_per_granule{0u};
    };

    class LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC CapacityDomainCatalog final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, ECapacityCatalogError> add(CapacityDomainDescriptor descriptor);
        [[nodiscard]] const CapacityDomainDescriptor* find(
            CapacityDomainIdView id) const noexcept;
        [[nodiscard]] std::span<const CapacityDomainDescriptor> all()
            const noexcept
        {
            return descriptors_;
        }

    private:
        std::vector<CapacityDomainDescriptor> descriptors_;
    };

    struct RuntimeCapacityDomainPlan final
    {
        CapacityDomainId domain;
        std::uint64_t requested{0u};
        std::uint64_t device_limit{0u};
        std::uint64_t protocol_limit{0u};
        std::uint64_t effective{0u};
        std::uint64_t estimated_bytes{0u};
        ECapacityPlanReason reason{ECapacityPlanReason::AUTO_DEFAULT};
    };

    struct RuntimeCapacityPlan final
    {
        RuntimeDeviceCapabilities device{};
        std::vector<RuntimeCapacityDomainPlan> domains;

        [[nodiscard]] LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC
        const RuntimeCapacityDomainPlan* find(
            CapacityDomainIdView domain) const noexcept;
        [[nodiscard]] LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC
        std::uint64_t effective(
            CapacityDomainIdView domain) const noexcept;
    };

    struct CapacityShortfall final
    {
        CapacityDomainId domain;
        ECapacityPlanError error{ECapacityPlanError::BUDGET_LIMIT};
        ECapacityPlanReason reason{ECapacityPlanReason::BUDGET_REJECT};
        std::uint64_t requested{0u};
        std::uint64_t effective{0u};
        std::uint64_t bytes{0u};
        std::uint64_t available_bytes{0u};
    };

    /// Trivially-copyable form carried by render operation replies. The hash is
    /// resolved through the already-frozen CapacityDomainCatalog; consumers
    /// still compare the catalog's full canonical name before using the ID.
    struct CapacityShortfallWire final
    {
        std::uint64_t domain_hash{0u};
        std::uint64_t requested{0u};
        std::uint64_t effective{0u};
        std::uint64_t bytes{0u};
        std::uint64_t available_bytes{0u};
        ECapacityPlanError error{ECapacityPlanError::BUDGET_LIMIT};
        ECapacityPlanReason reason{ECapacityPlanReason::BUDGET_REJECT};
        bool present{false};
    };
    static_assert(std::is_trivially_copyable_v<CapacityShortfallWire>);

    [[nodiscard]] inline CapacityShortfallWire capacityShortfallWire(
        const CapacityShortfall& shortfall) noexcept
    {
        return {
            shortfall.domain.hash(),
            shortfall.requested,
            shortfall.effective,
            shortfall.bytes,
            shortfall.available_bytes,
            shortfall.error,
            shortfall.reason,
            true};
    }

    /// Resolve every registered domain. Explicit requests are admitted first
    /// and never clamp. AUTO domains may shrink proportionally, but never below
    /// their registered minimum. Results are sorted by canonical domain name.
    [[nodiscard]] LUX_ENGINE_RESOURCE_DEPLOYMENT_PUBLIC
    lux::cxx::expected<RuntimeCapacityPlan, CapacityShortfall>
    makeRuntimeCapacityPlan(
        const RuntimeCapacityRequest& request,
        const RuntimeDeviceCapabilities& device,
        const CapacityDomainCatalog& catalog);
} // namespace lux::deployment
