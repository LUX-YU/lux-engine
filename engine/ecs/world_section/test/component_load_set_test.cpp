#include <lux/engine/ecs/ComponentLoadSet.hpp>

#include <lux/engine/meta/TypeStaticInfo.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace test
{
    struct Tag final
    {
    };

    struct Fixed final
    {
        std::uint32_t first{};
        std::uint64_t second{};
    };

    struct Variable final
    {
        std::string value;
    };
} // namespace test

namespace lux::meta
{
    template <>
    struct TypeStaticInfo<test::Tag>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::tuple{};
    };

    template <>
    struct TypeStaticInfo<test::Fixed>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Fixed::first>("first"),
            typeStaticField<&test::Fixed::second>("second")
        );
    };

    template <>
    struct TypeStaticInfo<test::Variable>
    {
        static constexpr bool available = true;
        static constexpr auto fields = std::make_tuple(
            typeStaticField<&test::Variable::value>("value")
        );
    };
} // namespace lux::meta

int main()
{
    const auto schemas = lux::ecs::ComponentSchemaSet::build({
        lux::ecs::makeComponentSchema<test::Tag>(
            lux::ecs::componentSchemaId("test.Tag")
        ),
        lux::ecs::makeComponentSchema<test::Fixed>(
            lux::ecs::componentSchemaId("test.Fixed")
        ),
        lux::ecs::makeComponentSchema<test::Variable>(
            lux::ecs::componentSchemaId("test.Variable")
        ),
    });
    assert(schemas);
    const auto* tag_schema = schemas->find(
        lux::ecs::componentSchemaId("test.Tag")
    );
    const auto* fixed_schema = schemas->find(
        lux::ecs::componentSchemaId("test.Fixed")
    );
    const auto* variable_schema = schemas->find(
        lux::ecs::componentSchemaId("test.Variable")
    );
    assert(tag_schema && fixed_schema && variable_schema);

    const std::array bindings{
        lux::ecs::bindComponentLoad<test::Tag>(*tag_schema),
        lux::ecs::bindComponentLoad<test::Fixed>(*fixed_schema),
        lux::ecs::bindComponentLoad<test::Variable>(*variable_schema),
    };
    assert(
        bindings[0].valueEncoding() ==
        lux::ecs::EWorldSectionValueEncoding::TAG
    );
    assert(
        bindings[1].valueEncoding() ==
        lux::ecs::EWorldSectionValueEncoding::FIXED
    );
    assert(bindings[1].fixedStride() == 12U);
    assert(
        bindings[2].valueEncoding() ==
        lux::ecs::EWorldSectionValueEncoding::VARIABLE
    );

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weak = lifetime;
    lux::ecs::ComponentLoadContribution contribution{
        lifetime,
        bindings
    };
    auto set = lux::ecs::ComponentLoadSet::build(
        *schemas,
        std::span(&contribution, 1U)
    );
    assert(set);
    assert(set->all().size() == 3U);
    assert(set->find(tag_schema->id) != nullptr);
    assert(set->find(fixed_schema->id)->fixedStride() == 12U);
    assert(set->find(variable_schema->id) != nullptr);
    lifetime.reset();
    contribution.code_lifetime.reset();
    assert(!weak.expired());

    const std::array duplicate_contributions{contribution, contribution};
    auto duplicate = lux::ecs::ComponentLoadSet::build(
        *schemas,
        duplicate_contributions
    );
    assert(!duplicate);
    assert(
        duplicate.error().code ==
        lux::ecs::EWorldSectionError::DUPLICATE_BINDING
    );

    const auto mismatched_schema =
        lux::ecs::makeComponentSchema<test::Fixed>(
            lux::ecs::componentSchemaId("test.Fixed"),
            2U
        );
    const std::array mismatched_bindings{
        lux::ecs::bindComponentLoad<test::Fixed>(mismatched_schema)
    };
    const std::array mismatched_contributions{
        lux::ecs::ComponentLoadContribution{{}, mismatched_bindings}
    };
    auto mismatched = lux::ecs::ComponentLoadSet::build(
        *schemas,
        mismatched_contributions
    );
    assert(!mismatched);
    assert(
        mismatched.error().code ==
        lux::ecs::EWorldSectionError::BINDING_MISMATCH
    );

    set = {};
    assert(weak.expired());
}
