#include <lux/engine/scene/SceneBuilder.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/SceneInstance.hpp>
#include <cassert>
#include <array>

using namespace lux;
struct Read
{
    static constexpr system::SystemTypeDescription Description{.canonical_name = "test.ability.read", .version = 1};
    virtual int value() const noexcept = 0;
};
struct Write
{
    static constexpr system::SystemTypeDescription Description{.canonical_name = "test.ability.write", .version = 1};
    virtual void set(int) noexcept = 0;
};
struct Dual final : Read, Write
{
    static constexpr std::array abilities{std::string_view{"test.ability.read"}, std::string_view{"test.ability.write"}};
    static constexpr system::SystemTypeDescription Description{
        .canonical_name = "test.dual", .version = 1, .capabilities = abilities};
    int current{42};
    int value() const noexcept override { return current; }
    void set(int value) noexcept override { current = value; }
};
struct Alternative final : Read
{
    static constexpr std::array abilities{std::string_view{"test.ability.read"}};
    static constexpr system::SystemTypeDescription Description{
        .canonical_name = "test.alternative", .version = 1, .capabilities = abilities};
    int value() const noexcept override { return 7; }
};
template<class T> scene::SceneSystemRegistration registration(std::span<const scene::SceneSystemCapabilityProjection> projections)
{
    return {.type = system::systemTypeId(T::Description.canonical_name), .cpp_type = cxx::typeToken<T>(),
        .description = &T::Description,
        .install = +[](scene::SceneBuilder &builder, scene::SceneSystemDescription input) noexcept
            -> cxx::expected<void, scene::SceneSystemBuildFailure> {
            const auto value = builder.emplaceSystem<T>(input.instanceId());
            if (!value) return cxx::unexpected(value.error());
            return {};
        }, .projections = projections};
}
int main()
{
    static constexpr std::array dual{scene::sceneSystemCapabilityProjection<Dual, Read>(),
                                     scene::sceneSystemCapabilityProjection<Dual, Write>()};
    static constexpr std::array alternative{scene::sceneSystemCapabilityProjection<Alternative, Read>()};
    const std::array registrations{registration<Dual>(dual), registration<Alternative>(alternative)};
    simulation::ecs::ComponentSchemaSet schemas;
    simulation::SimulationSystemRegistry simulation_types;
    for (std::size_t selected{}; selected < registrations.size(); ++selected)
    {
        scene::SceneDescriptionBuilder builder;
        assert(builder.addSystem({1}, "chosen-provider", registrations[selected].type, 1, {}, 0));
        auto description = std::move(builder).buildResolved();
        assert(description);
        auto created = scene::SceneInstance::create({
            std::make_shared<const scene::SceneDescription>(std::move(*description)),
            std::make_shared<const world::WorldDescription>(), std::make_shared<const simulation::SimulationDescription>(),
            schemas, simulation_types, registrations, {}});
        assert(created);
        auto *read = (*created)->findSceneSystem<Read>();
        assert(read && read->value() == (selected == 0 ? 42 : 7));
        auto *write = (*created)->findSceneSystem<Write>();
        if (selected == 0)
        {
            assert(write && read == static_cast<Read *>((*created)->findSceneSystem<Dual>()));
            assert(write == static_cast<Write *>((*created)->findSceneSystem<Dual>()));
            write->set(83);
            assert(read->value() == 83); // Both abilities reach one real instance.
        }
        else assert(!write);
    }
}
