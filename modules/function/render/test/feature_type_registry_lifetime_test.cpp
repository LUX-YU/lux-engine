#include <lux/engine/render/renderer/FeatureTypeRegistry.hpp>

#include <cstdio>
#include <memory>

namespace
{
    lux::render::Expected<lux::render::FeatureHandle> createProbe(void*, const void*, std::size_t) noexcept
    {
        return lux::render::FeatureHandle{1u, 1u};
    }

    bool check(bool condition, const char* message) noexcept
    {
        if (condition)
            return true;
        std::fprintf(stderr, "feature registry lifetime: %s\n", message);
        return false;
    }
}

int
main()
{
    using namespace lux::render;

    FeatureTypeRegistry registry;
    constexpr auto stable_type = featureId("test.DynamicLifetimeFeature");
    constexpr FeatureDescriptor descriptor{.type = stable_type, .name = "DynamicLifetimeFeature"};
    const FeatureFactory factory = makeSimpleFactory(&createProbe, "DynamicLifetimeFeature", descriptor);

    auto first_module = std::make_shared<int>(1);
    auto second_module = std::make_shared<int>(2);
    const std::weak_ptr<int> first_weak = first_module;
    const std::weak_ptr<int> second_weak = second_module;

    FeatureTypeRecord first{};
    first.factory = factory;
    first.registration_leases.emplace_back(first_module);
    auto first_add = registry.add(std::move(first));
    if (!check(first_add.has_value(), "initial registration failed"))
        return 1;

    FeatureTypeRecord second{};
    second.factory = factory;
    second.registration_leases.emplace_back(second_module);
    auto second_add = registry.add(std::move(second));
    if (!check(second_add.has_value(), "counted registration failed") ||
        !check(second_add->type_id == first_add->type_id, "same stable type did not share a registry record"))
        return 1;

    first_module.reset();
    second_module.reset();
    registry.noteInstanceAdded(stable_type);
    auto in_use = registry.release(first_add->type_id);
    if (!check(!in_use, "live feature instance did not reject unregister") ||
        !check(!first_weak.expired() && !second_weak.expired(), "rejected unregister released a module lease"))
        return 1;

    registry.noteInstanceRemoved(stable_type);
    auto shared_release = registry.release(first_add->type_id);
    if (!check(
            shared_release.has_value() && !shared_release->has_value(),
            "non-final unregister removed the shared record") ||
        !check(first_weak.expired() != second_weak.expired(), "non-final unregister did not release exactly one lease"))
        return 1;

    auto final_release = registry.release(first_add->type_id);
    const bool has_removed_record = final_release.has_value() && final_release->has_value();
    if (!check(has_removed_record, "final unregister did not return the removed record"))
        return 1;
    const bool has_retained_module = !first_weak.expired() || !second_weak.expired();
    if (!check(has_retained_module, "final record did not retain its module through dispatcher cleanup"))
        return 1;
    const bool removed_from_registry = !registry.contains(first_add->type_id);
    if (!check(removed_from_registry, "final unregister left the record installed"))
        return 1;

    final_release->reset();
    if (!check(first_weak.expired() && second_weak.expired(), "module lease outlived the removed factory record"))
        return 1;

    std::puts("feature registry lifetime: PASS");
    return 0;
}
