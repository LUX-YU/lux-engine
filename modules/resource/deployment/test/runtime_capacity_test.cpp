#include <lux/engine/resource/deployment/RuntimeCapacity.hpp>
#include <lux/engine/resource/deployment/RuntimeLaunchManifest.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace
{
    [[nodiscard]] lux::deployment::CapacityDomainId own(
        lux::deployment::CapacityDomainIdView id)
    {
        return lux::deployment::CapacityDomainId{id.name()};
    }

    [[nodiscard]] lux::deployment::CapacityDomainCatalog makeCatalog(
        std::uint64_t instance_device_limit = 100'000u)
    {
        using namespace lux::deployment;
        CapacityDomainCatalog catalog;
        assert(catalog.add(CapacityDomainDescriptor{
            .id = own(kActiveRenderInstancesCapacity),
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
    using namespace lux::deployment;

    RuntimeDeviceCapabilities device{};
    device.vram_budget_bytes = 1ull << 30u;
    device.max_storage_buffer_range = 8ull << 20u;

    RuntimeCapacityRequest exact{};
    exact.set(
        kActiveRenderInstancesCapacity,
        RuntimeCapacityValue::exact(100'000u));
    exact.set(
        kClassicMeshRecordsCapacity,
        RuntimeCapacityValue::exact(100'000u));
    const auto catalog = makeCatalog();
    const auto exact_plan = makeRuntimeCapacityPlan(exact, device, catalog);
    assert(exact_plan);
    assert(exact_plan->effective(kActiveRenderInstancesCapacity) == 100'000u);
    assert(exact_plan->effective(kClassicMeshRecordsCapacity) == 100'000u);
    assert(exact_plan->find(kActiveRenderInstancesCapacity)->reason ==
        ECapacityPlanReason::REQUESTED);

    const auto device_catalog = makeCatalog(150'000u);
    auto device_limited = exact;
    device_limited.set(
        kActiveRenderInstancesCapacity,
        RuntimeCapacityValue::exact(200'000u));
    const auto device_failure = makeRuntimeCapacityPlan(
        device_limited,
        device,
        device_catalog);
    assert(!device_failure);
    assert(lux::extensions::sameStableId(
        device_failure.error().domain.view(),
        kActiveRenderInstancesCapacity));
    assert(device_failure.error().reason ==
        ECapacityPlanReason::DEVICE_CLAMP);

    CapacityDomainCatalog protocol_catalog;
    assert(protocol_catalog.add(CapacityDomainDescriptor{
        .id = own(kActiveRenderInstancesCapacity),
        .device_limit = 0xffffffffull,
        .protocol_limit = 1u << 20u,
        .automatic_target = 65'536u,
        .minimum = 1'000u,
        .units_per_granule = 16'384u,
        .bytes_per_granule = 16'384u * 280u,
    }));
    RuntimeCapacityRequest protocol_limited;
    protocol_limited.set(
        kActiveRenderInstancesCapacity,
        RuntimeCapacityValue::exact((1u << 20u) + 1u));
    const auto protocol_failure = makeRuntimeCapacityPlan(
        protocol_limited,
        device,
        protocol_catalog);
    assert(!protocol_failure);
    assert(protocol_failure.error().reason ==
        ECapacityPlanReason::PROTOCOL_CLAMP);
    assert(protocol_failure.error().effective == (1u << 20u));

    auto small_budget = device;
    small_budget.vram_budget_bytes = 10ull << 20u;
    const auto budget_failure = makeRuntimeCapacityPlan(
        exact,
        small_budget,
        catalog);
    assert(!budget_failure);
    assert(budget_failure.error().reason ==
        ECapacityPlanReason::BUDGET_REJECT);
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
    assert(budget_wire.reason == ECapacityPlanReason::BUDGET_REJECT);

    RuntimeCapacityRequest automatic{};
    const auto narrow_catalog = makeCatalog(1'000u);
    const auto auto_plan = makeRuntimeCapacityPlan(
        automatic,
        device,
        narrow_catalog);
    assert(auto_plan);
    assert(auto_plan->effective(kActiveRenderInstancesCapacity) == 1'000u);
    assert(auto_plan->find(kActiveRenderInstancesCapacity)->reason ==
        ECapacityPlanReason::DEVICE_CLAMP);

    auto budget_clamped_device = device;
    budget_clamped_device.vram_budget_bytes = 40ull << 20u;
    const auto budget_clamped = makeRuntimeCapacityPlan(
        automatic,
        budget_clamped_device,
        catalog);
    assert(budget_clamped);
    assert(budget_clamped->find(kActiveRenderInstancesCapacity)->reason ==
        ECapacityPlanReason::BUDGET_REJECT);
    assert(budget_clamped->find(kClassicMeshRecordsCapacity)->reason ==
        ECapacityPlanReason::BUDGET_REJECT);
    std::uint64_t admitted_bytes = 0u;
    for (const auto& domain : budget_clamped->domains)
        admitted_bytes += domain.estimated_bytes;
    assert(admitted_bytes <=
        budget_clamped_device.vram_budget_bytes -
            budget_clamped_device.vram_budget_bytes / 10u);

    auto exhausted_device = device;
    exhausted_device.vram_budget_bytes = 1u;
    exhausted_device.vram_usage_bytes = 1u;
    const auto exhausted = makeRuntimeCapacityPlan(
        automatic,
        exhausted_device,
        catalog);
    assert(!exhausted);
    assert(exhausted.error().reason == ECapacityPlanReason::BUDGET_REJECT);
    assert(exhausted.error().available_bytes == 0u);

    RuntimeCapacityRequest unknown{};
    unknown.set(
        capacityDomainId("lux.test.unknown"),
        RuntimeCapacityValue::exact(1u));
    const auto unknown_result = makeRuntimeCapacityPlan(
        unknown,
        device,
        catalog);
    assert(!unknown_result);
    assert(unknown_result.error().error ==
        ECapacityPlanError::UNKNOWN_DOMAIN);

    const auto manifest_path = std::filesystem::temp_directory_path() /
        ("lux-runtime-capacity-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".toml");
    RuntimeLaunchManifest manifest;
    manifest.game_pak = "game.pak";
    manifest.capacity.set(
        kActiveRenderInstancesCapacity,
        RuntimeCapacityValue::exact(100'000u));
    assert(manifest.saveToFile(manifest_path));
    const auto roundtrip = RuntimeLaunchManifest::loadFromFile(manifest_path);
    assert(roundtrip);
    const auto* roundtrip_instances = roundtrip->capacity.find(
        kActiveRenderInstancesCapacity);
    assert(roundtrip_instances);
    assert(roundtrip_instances->mode == ECapacityRequestMode::EXPLICIT);
    assert(roundtrip_instances->value == 100'000u);
    assert(!roundtrip->capacity.find(kClassicMeshRecordsCapacity));
    std::error_code remove_error;
    std::filesystem::remove(manifest_path, remove_error);

    manifest.capacity.set(
        kActiveRenderInstancesCapacity,
        RuntimeCapacityValue::exact(
            std::numeric_limits<std::uint64_t>::max()));
    assert(!manifest.saveToFile(manifest_path));
    return 0;
}
