#pragma once

#include <lux/engine/function/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/StableNameId.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lux::render
{
    struct CapacityDomainIdTag final
    {
    };
    using CapacityDomainIdView = lux::cxx::StableNameIdView<CapacityDomainIdTag>;
    using CapacityDomainId = lux::cxx::StableNameId<CapacityDomainIdTag>;

    [[nodiscard]] constexpr CapacityDomainIdView capacityId(std::string_view name) noexcept
    {
        return CapacityDomainIdView{name};
    }

    [[nodiscard]] LUX_FUNCTION_PUBLIC bool isValidCapacityDomainName(std::string_view name) noexcept;

    inline constexpr auto kActiveInstancesCapacity = capacityId("lux.render.instances");
    inline constexpr auto kClassicMeshRecordsCapacity = capacityId("lux.render.classic_mesh.records");
    inline constexpr auto kClassicMeshGeometryBytesCapacity = capacityId("lux.render.classic_mesh.geometry_bytes");

    enum class CapacityRequestMode : std::uint8_t
    {
        AUTO,
        EXPLICIT
    };

    enum class CapacityPlanReason : std::uint8_t
    {
        AUTO_DEFAULT,
        REQUESTED,
        DEVICE_CLAMP,
        PROTOCOL_CLAMP,
        BUDGET_REJECT
    };

    enum class CapacityCatalogError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        DUPLICATE_DOMAIN,
        ID_COLLISION,
    };

    enum class CapacityPlanError : std::uint8_t
    {
        INVALID_REQUEST,
        DUPLICATE_REQUEST,
        UNKNOWN_DOMAIN,
        DEVICE_LIMIT,
        PROTOCOL_LIMIT,
        BUDGET_LIMIT,
    };

    struct CapacityValue final
    {
        CapacityRequestMode mode{CapacityRequestMode::AUTO};
        std::uint64_t value{0u};

        [[nodiscard]] static constexpr CapacityValue automatic() noexcept
        {
            return {};
        }

        [[nodiscard]] static constexpr CapacityValue exact(std::uint64_t requested) noexcept
        {
            return {CapacityRequestMode::EXPLICIT, requested};
        }
    };

    struct CapacityRequestEntry final
    {
        CapacityDomainId domain;
        CapacityValue value{};
    };

    /// Recipe-owned requests. Absence means AUTO. Programmatic composition may
    /// use set(); disk decoders must reject duplicate keys before calling it.
    struct CapacityRequest final
    {
        std::vector<CapacityRequestEntry> domains;

        LUX_FUNCTION_PUBLIC void set(CapacityDomainIdView domain, CapacityValue value);
        [[nodiscard]] LUX_FUNCTION_PUBLIC const CapacityValue* find(CapacityDomainIdView domain) const noexcept;
    };

    /// Raw device facts. Domain-specific limits are contributed separately to
    /// CapacityCatalog before the immutable plan is resolved.
    struct CapacityDeviceFacts final
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

    class LUX_FUNCTION_PUBLIC CapacityCatalog final
    {
    public:
        [[nodiscard]] lux::cxx::expected<void, CapacityCatalogError> add(CapacityDomainDescriptor descriptor);
        [[nodiscard]] const CapacityDomainDescriptor* find(CapacityDomainIdView id) const noexcept;
        [[nodiscard]] std::span<const CapacityDomainDescriptor> all() const noexcept
        {
            return descriptors_;
        }

    private:
        std::vector<CapacityDomainDescriptor> descriptors_;
    };

    struct CapacityDomainPlan final
    {
        CapacityDomainId domain;
        std::uint64_t requested{0u};
        std::uint64_t device_limit{0u};
        std::uint64_t protocol_limit{0u};
        std::uint64_t effective{0u};
        std::uint64_t estimated_bytes{0u};
        CapacityPlanReason reason{CapacityPlanReason::AUTO_DEFAULT};
    };

    struct CapacityPlan final
    {
        CapacityDeviceFacts device{};
        std::vector<CapacityDomainPlan> domains;

        [[nodiscard]] LUX_FUNCTION_PUBLIC const CapacityDomainPlan* find(CapacityDomainIdView domain) const noexcept;
        [[nodiscard]] LUX_FUNCTION_PUBLIC std::uint64_t effective(CapacityDomainIdView domain) const noexcept;
    };

    struct CapacityShortfall final
    {
        CapacityDomainId domain;
        CapacityPlanError error{CapacityPlanError::BUDGET_LIMIT};
        CapacityPlanReason reason{CapacityPlanReason::BUDGET_REJECT};
        std::uint64_t requested{0u};
        std::uint64_t effective{0u};
        std::uint64_t bytes{0u};
        std::uint64_t available_bytes{0u};
    };

    /// Trivially-copyable form carried by render operation replies. The hash is
    /// resolved through the already-frozen CapacityCatalog; consumers
    /// still compare the catalog's full canonical name before using the ID.
    struct CapacityShortfallWire final
    {
        std::uint64_t domain_hash{0u};
        std::uint64_t requested{0u};
        std::uint64_t effective{0u};
        std::uint64_t bytes{0u};
        std::uint64_t available_bytes{0u};
        CapacityPlanError error{CapacityPlanError::BUDGET_LIMIT};
        CapacityPlanReason reason{CapacityPlanReason::BUDGET_REJECT};
        bool present{false};
    };
    static_assert(std::is_trivially_copyable_v<CapacityShortfallWire>);

    [[nodiscard]] inline CapacityShortfallWire capacityShortfallWire(const CapacityShortfall& shortfall) noexcept
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
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<CapacityPlan, CapacityShortfall>
    makeCapacityPlan(const CapacityRequest& request, const CapacityDeviceFacts& device, const CapacityCatalog& catalog);
} // namespace lux::render
