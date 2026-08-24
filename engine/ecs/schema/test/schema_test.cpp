#include "GeneratedSchemaProbe.hpp"

#include <lux/engine/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ecs/reflection/ComponentReflectionAdapter.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

std::span<const lux::ecs::ComponentSchema>
lux_ecs_schema_probe_component_schemas() noexcept;

namespace
{
    struct Position final
    {
        int value{};
    };

    struct DerivedCache final
    {
        int value{};
    };

    struct NonCopy final
    {
        NonCopy() = default;
        NonCopy(const NonCopy&) = delete;
        NonCopy& operator=(const NonCopy&) = delete;
    };

    struct RebuildPersistent final
    {
        int value{};
    };

    lux::cxx::expected<void, lux::ecs::EComponentCodecError> encodeRebuild(
        const lux::ecs::ComponentSchema&,
        const lux::ecs::World&,
        lux::ecs::Entity,
        lux::ecs::ComponentEncodePort&
    ) noexcept
    {
        return {};
    }

    lux::cxx::expected<void, lux::ecs::EComponentCodecError> decodeRebuild(
        const lux::ecs::ComponentSchema&,
        lux::ecs::WorldEdit& edit,
        lux::ecs::Entity entity,
        std::uint32_t,
        lux::ecs::ComponentDecodePort&
    ) noexcept
    {
        edit.emplace<RebuildPersistent>(entity);
        return {};
    }

    struct ReflectedValue final
    {
        std::int32_t count{};
        float weight{};
        std::string label;
        lux::ecs::Entity target{lux::ecs::NullEntity};
    };

    struct Property final
    {
        std::string name;
        lux::ecs::EComponentWireType type{};
        std::vector<std::byte> bytes;
    };

    class EncodePort final : public lux::ecs::ComponentEncodePort
    {
      public:
        lux::cxx::expected<void, lux::ecs::EComponentCodecError> write(
            std::string_view name,
            lux::ecs::EComponentWireType type,
            std::span<const std::byte> bytes
        ) noexcept override
        {
            try
            {
                properties.push_back(Property{
                    std::string{name},
                    type,
                    std::vector<std::byte>{bytes.begin(), bytes.end()},
                });
                return {};
            }
            catch (...)
            {
                return lux::cxx::unexpected(
                    lux::ecs::EComponentCodecError::ALLOCATION_FAILURE
                );
            }
        }

        lux::cxx::expected<void, lux::ecs::EComponentCodecError> writeEntity(
            std::string_view name,
            lux::ecs::Entity entity
        ) noexcept override
        {
            return write(
                name,
                lux::ecs::EComponentWireType::LOCAL_ENTITY,
                std::as_bytes(std::span{&entity, 1})
            );
        }

        lux::cxx::expected<void, lux::ecs::EComponentCodecError>
        writeStableReference(
            std::string_view,
            std::span<const std::byte>
        ) noexcept override
        {
            return lux::cxx::unexpected(
                lux::ecs::EComponentCodecError::INVALID_DATA
            );
        }

        std::vector<Property> properties;
    };

    class DecodePort final : public lux::ecs::ComponentDecodePort
    {
      public:
        explicit DecodePort(std::span<const Property> properties) noexcept
            : properties_(properties)
        {}

        bool next(lux::ecs::EncodedPropertyView& property) noexcept override
        {
            if (index_ == properties_.size())
                return false;
            const auto& source = properties_[index_++];
            property = {source.name, source.type, source.bytes};
            return true;
        }

        lux::cxx::expected<lux::ecs::Entity, lux::ecs::EComponentCodecError>
        resolveEntity(std::span<const std::byte> encoded) const noexcept override
        {
            if (encoded.size() != sizeof(lux::ecs::Entity))
                return lux::cxx::unexpected(
                    lux::ecs::EComponentCodecError::INVALID_DATA
                );
            lux::ecs::Entity entity{};
            std::memcpy(&entity, encoded.data(), sizeof(entity));
            return entity;
        }

        lux::cxx::expected<
            std::array<std::byte, 16>,
            lux::ecs::EComponentCodecError>
        resolveStableReference(
            std::span<const std::byte>
        ) const noexcept override
        {
            return lux::cxx::unexpected(
                lux::ecs::EComponentCodecError::UNKNOWN_REFERENCE
            );
        }

      private:
        std::span<const Property> properties_;
        std::size_t index_{};
    };
}

int main()
{
    lux::meta::meta_module_init();
    const auto generated = lux_ecs_schema_probe_component_schemas();
    assert(generated.size() == 4u);
    const auto find_generated = [&](std::string_view name)
        -> const lux::ecs::ComponentSchema*
    {
        const auto id = lux::ecs::componentSchemaId(name);
        for (const auto& schema : generated)
        {
            if (schema.id == id)
                return &schema;
        }
        return nullptr;
    };
    const auto* explicit_copy = find_generated("test.generated-probe");
    assert(explicit_copy != nullptr && explicit_copy->version == 7u);
    assert(explicit_copy->snapshot ==
        lux::ecs::EComponentSnapshotPolicy::COPY);
    assert(explicit_copy->codec.present());
    assert(explicit_copy->codec.context != nullptr);
    const auto* rebuild_reflected = find_generated(
        "test.generated-rebuild-reflected"
    );
    assert(rebuild_reflected != nullptr);
    assert(rebuild_reflected->snapshot ==
        lux::ecs::EComponentSnapshotPolicy::REBUILD);
    assert(rebuild_reflected->codec.present());
    const auto* rebuild_none = find_generated(
        "test.generated-rebuild-none"
    );
    assert(rebuild_none != nullptr);
    assert(rebuild_none->snapshot ==
        lux::ecs::EComponentSnapshotPolicy::REBUILD);
    assert(!rebuild_none->codec.present());
    auto module_lifetime = std::make_shared<int>(9);
    std::weak_ptr<int> module_weak = module_lifetime;
    auto generated_set = lux::ecs::ComponentSchemaSet::build(
        generated,
        module_lifetime
    );
    assert(generated_set);
    auto pinned_generated = std::move(*generated_set);
    module_lifetime.reset();
    assert(!module_weak.expired());

    auto lifetime = std::make_shared<int>(42);
    auto position = lux::ecs::makeComponentSchema<Position>(
        lux::ecs::componentSchemaId("test.position"),
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY,
        {},
        lifetime
    );
    auto cache = lux::ecs::makeComponentSchema<DerivedCache>(
        lux::ecs::componentSchemaId("test.derived_cache"),
        1,
        lux::ecs::EComponentSnapshotPolicy::REBUILD
    );

    auto built = lux::ecs::ComponentSchemaSet::build({position});
    assert(built);
    const auto* stable_pointer = built->find(position.id);
    assert(stable_pointer != nullptr);
    assert(stable_pointer->code_lifetime == lifetime);

    auto extended = built->extended(std::span<const lux::ecs::ComponentSchema>(&cache, 1));
    assert(extended);
    assert(extended->all().size() == 2);
    assert(built->all().size() == 1);
    assert(built->find(position.id) == stable_pointer);

    auto duplicate = lux::ecs::ComponentSchemaSet::build({position, position});
    assert(!duplicate);
    assert(duplicate.error().code == lux::ecs::ESchemaError::DUPLICATE_SCHEMA_ID);

    auto invalid_copy_schema = lux::ecs::makeComponentSchema<NonCopy>(
        lux::ecs::componentSchemaId("test.non-copy"),
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY
    );
    auto invalid_copy = lux::ecs::ComponentSchemaSet::build(
        {invalid_copy_schema}
    );
    assert(!invalid_copy);
    assert(invalid_copy.error().code == lux::ecs::ESchemaError::COPY_WITHOUT_CLONE);

    lux::meta::RefClass reflection;
    reflection.fields = {
        lux::meta::RefField{
            .name = "count",
            .type = lux::meta::ref_type_of_v<std::int32_t>,
            .offset = static_cast<std::uint32_t>(offsetof(ReflectedValue, count)),
        },
        lux::meta::RefField{
            .name = "weight",
            .type = lux::meta::ref_type_of_v<float>,
            .offset = static_cast<std::uint32_t>(offsetof(ReflectedValue, weight)),
        },
        lux::meta::RefField{
            .name = "label",
            .type = lux::meta::ref_type_of_v<std::string>,
            .offset = static_cast<std::uint32_t>(offsetof(ReflectedValue, label)),
        },
        lux::meta::RefField{
            .name = "target",
            .type = lux::meta::ref_type_of_v<lux::ecs::Entity>,
            .offset = static_cast<std::uint32_t>(offsetof(ReflectedValue, target)),
        },
    };
    auto reflected_schema = lux::ecs::makeGeneratedComponentSchema<ReflectedValue>(
        reflection,
        "test.reflected",
        1,
        lux::ecs::EComponentSnapshotPolicy::COPY,
        lux::ecs::EGeneratedComponentCodec::REFLECTED
    );
    assert(reflected_schema.codec.present());
    auto rebuild_with_codec = lux::ecs::makeComponentSchema<RebuildPersistent>(
        lux::ecs::componentSchemaId("test.rebuild-persistent"),
        1,
        lux::ecs::EComponentSnapshotPolicy::REBUILD,
        lux::ecs::ComponentCodec{&encodeRebuild, &decodeRebuild}
    );
    auto policy_matrix = lux::ecs::ComponentSchemaSet::build(
        {position, cache, reflected_schema, rebuild_with_codec}
    );
    assert(policy_matrix);

    lux::ecs::World source;
    auto source_edit_result = source.edit();
    assert(source_edit_result);
    auto source_edit = std::move(*source_edit_result);
    const auto source_entity = source_edit.create();
    const auto source_target = source_edit.create();
    source_edit.emplace<ReflectedValue>(
        source_entity,
        ReflectedValue{17, 2.5F, "tagged", source_target}
    );
    source_edit = {};

    EncodePort encoded;
    assert(reflected_schema.codec.encode(
        reflected_schema,
        source,
        source_entity,
        encoded
    ));
    assert(encoded.properties.size() == 4u);

    lux::ecs::World target;
    auto target_edit_result = target.edit();
    assert(target_edit_result);
    auto target_edit = std::move(*target_edit_result);
    const auto target_entity = target_edit.create();
    DecodePort decoder{encoded.properties};
    assert(reflected_schema.codec.decode(
        reflected_schema,
        target_edit,
        target_entity,
        1,
        decoder
    ));
    const auto& round_trip = target.get<ReflectedValue>(target_entity);
    assert(round_trip.count == 17);
    assert(round_trip.weight == 2.5F);
    assert(round_trip.label == "tagged");
    assert(round_trip.target == source_target);
    pinned_generated = {};
    assert(module_weak.expired());
    lux::meta::meta_module_deinit();
}
