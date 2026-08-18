#include <lux/engine/function/render/Capacity.hpp>

#include <cassert>
#include <cstdint>

namespace
{
    [[nodiscard]] lux::render::CapacityDomainId own(
        lux::render::CapacityDomainIdView id)
    {
        return lux::render::CapacityDomainId{id.name()};
    }

    [[nodiscard]] lux::render::CapacityCatalog makeCatalog(
        std::uint64_t instance_device_limit = 100'000u)
    {
        using namespace lux::render;
        CapacityCatalog catalog;
        assert(catalog.add(CapacityDomainDescriptor{
            .id = own(kActiveInstancesCapacity),
            .device_limit = instance_device_limit,
            .protocol_limit = 0xffffffffull,
            .automatic_target = 65'536u,
            .minimum = 1'000u,
            .units_per_granule = 16'384u,
            .bytes_per_granule = 16'384u * 280u,
        }));
        assert(catalog.add(CapacityDomainDescriptor{
            .id = own(kClassicMeshRecordsCapacity),
            .device_limit = 0xffffffffull,
            .protocol_limit = 0xffffffffull,
            .automatic_target = 65'536u,
            .minimum = 4'096u,
            .units_per_granule = 4'096u,
            .bytes_per_granule = 4'096u * 512u,
        }));
        return catalog;
    }
} // namespace

int main()
{
    using namespace lux::render;

    CapacityDeviceFacts device{};
    device.vram_budget_bytes = 1ull << 30u;
    device.max_storage_buffer_range = 8ull << 20u;

    CapacityRequest exact{};
    exact.set(
        kActiveInstancesCapacity,
        CapacityValue::exact(100'000u));
    exact.set(
        kClassicMeshRecordsCapacity,
        CapacityValue::exact(100'000u));
    const auto catalog = makeCatalog();

    CapacityCatalog invalid_catalog;
    const auto invalid_domain = invalid_catalog.add(CapacityDomainDescriptor{
        .id = CapacityDomainId{"Lux.render.invalid"},
        .device_limit = 1u,
        .protocol_limit = 1u,
        .automatic_target = 1u,
        .minimum = 1u,
        .units_per_granule = 1u,
        .bytes_per_granule = 1u,
    });
    assert(!invalid_domain);
    assert(invalid_domain.error() ==
        CapacityCatalogError::INVALID_DESCRIPTOR);

    auto duplicate_catalog = makeCatalog();
    const auto duplicate_domain = duplicate_catalog.add(
        CapacityDomainDescriptor{
            .id = own(kActiveInstancesCapacity),
            .device_limit = 100'000u,
            .protocol_limit = 0xffffffffull,
            .automatic_target = 65'536u,
            .minimum = 1'000u,
            .units_per_granule = 16'384u,
            .bytes_per_granule = 16'384u * 280u,
        });
    assert(!duplicate_domain);
    assert(duplicate_domain.error() ==
        CapacityCatalogError::DUPLICATE_DOMAIN);

    const auto exact_plan = makeCapacityPlan(exact, device, catalog);
    assert(exact_plan);
    assert(exact_plan->effective(kActiveInstancesCapacity) == 100'000u);
    assert(exact_plan->effective(kClassicMeshRecordsCapacity) == 100'000u);
    assert(exact_plan->find(kActiveInstancesCapacity)->reason ==
        CapacityPlanReason::REQUESTED);

    const auto device_catalog = makeCatalog(150'000u);
    auto device_limited = exact;
    device_limited.set(
        kActiveInstancesCapacity,
        CapacityValue::exact(200'000u));
    const auto device_failure = makeCapacityPlan(
        device_limited,
        device,
        device_catalog);
    assert(!device_failure);
    assert(device_failure.error().domain.view() ==
        kActiveInstancesCapacity);
    assert(device_failure.error().reason ==
        CapacityPlanReason::DEVICE_CLAMP);

    CapacityCatalog protocol_catalog;
    assert(protocol_catalog.add(CapacityDomainDescriptor{
        .id = own(kActiveInstancesCapacity),
        .device_limit = 0xffffffffull,
        .protocol_limit = 1u << 20u,
        .automatic_target = 65'536u,
        .minimum = 1'000u,
        .units_per_granule = 16'384u,
        .bytes_per_granule = 16'384u * 280u,
    }));
    CapacityRequest protocol_limited;
    protocol_limited.set(
        kActiveInstancesCapacity,
        CapacityValue::exact((1u << 20u) + 1u));
    const auto protocol_failure = makeCapacityPlan(
        protocol_limited,
        device,
        protocol_catalog);
    assert(!protocol_failure);
    assert(protocol_failure.error().reason ==
        CapacityPlanReason::PROTOCOL_CLAMP);
    assert(protocol_failure.error().effective == (1u << 20u));

    auto small_budget = device;
    small_budget.vram_budget_bytes = 10ull << 20u;
    const auto budget_failure = makeCapacityPlan(
        exact,
        small_budget,
        catalog);
    assert(!budget_failure);
    assert(budget_failure.error().reason ==
        CapacityPlanReason::BUDGET_REJECT);
    assert(budget_failure.error().bytes >
        budget_failure.error().available_bytes);
    const auto budget_wire = capacityShortfallWire(budget_failure.error());
    assert(budget_wire.present);
    assert(budget_wire.domain_hash == budget_failure.error().domain.hash());
    assert(budget_wire.requested == budget_failure.error().requested);
    assert(budget_wire.effective == budget_failure.error().effective);
    assert(budget_wire.bytes == budget_failure.error().bytes);
    assert(budget_wire.available_bytes ==
        budget_failure.error().available_bytes);
    assert(budget_wire.reason == CapacityPlanReason::BUDGET_REJECT);

    CapacityRequest automatic{};
    const auto narrow_catalog = makeCatalog(1'000u);
    const auto auto_plan = makeCapacityPlan(
        automatic,
        device,
        narrow_catalog);
    assert(auto_plan);
    assert(auto_plan->effective(kActiveInstancesCapacity) == 1'000u);
    assert(auto_plan->find(kActiveInstancesCapacity)->reason ==
        CapacityPlanReason::DEVICE_CLAMP);

    auto budget_clamped_device = device;
    budget_clamped_device.vram_budget_bytes = 40ull << 20u;
    const auto budget_clamped = makeCapacityPlan(
        automatic,
        budget_clamped_device,
        catalog);
    assert(budget_clamped);
    assert(budget_clamped->find(kActiveInstancesCapacity)->reason ==
        CapacityPlanReason::BUDGET_REJECT);
    assert(budget_clamped->find(kClassicMeshRecordsCapacity)->reason ==
        CapacityPlanReason::BUDGET_REJECT);
    std::uint64_t admitted_bytes = 0u;
    for (const auto& domain : budget_clamped->domains)
        admitted_bytes += domain.estimated_bytes;
    assert(admitted_bytes <=
        budget_clamped_device.vram_budget_bytes -
            budget_clamped_device.vram_budget_bytes / 10u);

    auto exhausted_device = device;
    exhausted_device.vram_budget_bytes = 1u;
    exhausted_device.vram_usage_bytes = 1u;
    const auto exhausted = makeCapacityPlan(
        automatic,
        exhausted_device,
        catalog);
    assert(!exhausted);
    assert(exhausted.error().reason == CapacityPlanReason::BUDGET_REJECT);
    assert(exhausted.error().available_bytes == 0u);

    CapacityRequest unknown{};
    unknown.set(
        capacityId("lux.test.unknown"),
        CapacityValue::exact(1u));
    const auto unknown_result = makeCapacityPlan(
        unknown,
        device,
        catalog);
    assert(!unknown_result);
    assert(unknown_result.error().error ==
        CapacityPlanError::UNKNOWN_DOMAIN);

    return 0;
}
