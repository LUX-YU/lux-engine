#include <cassert>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/simulation/ecs/ComponentDecode.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/process/TaskScope.hpp>

void decodedValues(const lux::simulation::ecs::ComponentSchema &schema)
{
    using namespace lux::simulation::ecs;
    assert(schema.decode_value);
    Registry registry;
    const auto existing = registry.create();
    WorldEntityMap identities;
    const auto id = [](const char *text) { return lux::world::WorldObjectId{*uuids::uuid::from_string(text)}; };
    const auto a = id("80000000-0000-0000-0000-000000000001");
    const auto b = id("80000000-0000-0000-0000-000000000002");
    const auto c = id("80000000-0000-0000-0000-000000000003");
    assert(identities.bind(c, existing));
    auto plan = planEntityCreation(registry, 2);
    assert(plan && plan->entities().size() == 2);
    assert(identities.bind(a, plan->entities()[0]) && identities.bind(b, plan->entities()[1]));
    const ComponentEntityResolver resolver{
        &identities,
        [](const void *state,
           lux::world::WorldObjectId identity) noexcept -> lux::cxx::expected<Entity, ComponentDecodeFailure>
        {
            const auto entity = static_cast<const WorldEntityMap *>(state)->entity(identity);
            if (identity.valid() && entity == NullEntity)
            {
                return lux::cxx::unexpected(
                    ComponentDecodeFailure{EComponentDecodeError::UNRESOLVED_REFERENCE, 0, identity});
            }
            return entity;
        }};
    const auto parent = directComponentDecodeValue<Parent, 1>();
    assert(parent);
    const auto encode = [&](Entity entity)
    {
        std::vector<std::byte> bytes;
        lux::serialization::BinaryWriter binary(bytes);
        WorldComponentArchive writer(binary, identities);
        assert(
            lux::serialization::write(writer, Parent{entity}, lux::serialization::SerializationBudget{1024, 1024, 64}));
        return bytes;
    };
    const auto before = std::as_const(registry).storage<Entity>()->free_list();
    const auto a_bytes = encode(plan->entities()[1]);
    const auto b_bytes = encode(plan->entities()[0]);
    auto first = parent(1, a_bytes, resolver, {});
    auto broken = b_bytes;
    broken.pop_back();
    auto rejected = parent(1, broken, resolver, {});
    assert(first && !rejected && rejected.error().code == EComponentDecodeError::MALFORMED_PAYLOAD);
    assert(std::as_const(registry).storage<Entity>()->free_list() == before && validateEntityCreation(registry, *plan));
    auto second = parent(1, b_bytes, resolver, {});
    assert(second);
    for (const auto entity : plan->entities())
    {
        assert(registry.create(entity) == entity);
    }
    std::move(*first).installInto(registry, plan->entities()[0]);
    std::move(*second).installInto(registry, plan->entities()[1]);
    assert(registry.get<Parent>(plan->entities()[0]).entity == plan->entities()[1]);
    assert(registry.get<Parent>(plan->entities()[1]).entity == plan->entities()[0]);
    assert(!validateEntityCreation(registry, *plan));
    auto existing_value = parent(1, encode(existing), resolver, {});
    auto null_value = parent(1, encode(NullEntity), resolver, {});
    assert(existing_value && null_value);
    using NestedReferences = std::vector<std::vector<Parent>>;
    const NestedReferences references{{Parent{plan->entities()[1]}, Parent{existing}}, {Parent{NullEntity}}};
    std::vector<std::byte> nested_bytes;
    lux::serialization::BinaryWriter nested_binary(nested_bytes);
    WorldComponentArchive nested_writer(nested_binary, identities);
    assert(
        lux::serialization::write(nested_writer, references, lux::serialization::SerializationBudget{4096, 4096, 64}));
    const auto nested_decode = directComponentDecodeValue<NestedReferences, 1>();
    assert(nested_decode);
    auto nested = nested_decode(1, nested_bytes, resolver, {});
    assert(nested);
    std::move(*nested).installInto(registry, existing);
    const auto &restored_references = registry.get<NestedReferences>(existing);
    assert(restored_references[0][0].entity == plan->entities()[1]);
    assert(restored_references[0][1].entity == existing && restored_references[1][0].entity == NullEntity);
    identities.unbind(plan->entities()[1]);
    auto unresolved_nested = nested_decode(1, nested_bytes, resolver, {});
    assert(!unresolved_nested && unresolved_nested.error().code == EComponentDecodeError::UNRESOLVED_REFERENCE &&
           unresolved_nested.error().reference == b);
    std::printf("C05 nested references: existing/null/planned resolved; rejected identity retained at offset=%zu\n",
                unresolved_nested.error().offset);
    auto unresolved = parent(1, a_bytes, resolver, {});
    assert(!unresolved && unresolved.error().code == EComponentDecodeError::UNRESOLVED_REFERENCE &&
           unresolved.error().reference == b && unresolved.error().offset == 0);
    const auto old = plan->entities()[0];
    registry.destroy(old);
    auto reused = planEntityCreation(registry, 1);
    assert(reused && reused->entities()[0] != old);

    registry.emplace<consumer::Component>(existing);
    auto capture = schema.capture(registry, existing, {});
    auto encoded = capture->encode(identities, 1024 * 1024);
    assert(encoded);
    auto code = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = code;
    auto value = schema.decode_value(1, *encoded, resolver, code);
    assert(value && value->type() == schema.cpp_type && value->accountedBytes() >= sizeof(consumer::Component));
    code.reset();
    assert(!lifetime.expired());
    const auto destination = registry.create();
    std::move(*value).installInto(registry, destination);
    assert(lifetime.expired() && value->accountedBytes() == 0);
    assert(registry.get<consumer::Component>(destination).sequence.front().name == "Unicode 中文");
    int destroyed{}, code_released{};
    struct Temporary final
    {
        int *destroyed;
        explicit Temporary(int &count) : destroyed(&count)
        {
        }
        Temporary(Temporary &&other) noexcept : destroyed(std::exchange(other.destroyed, nullptr))
        {
        }
        ~Temporary()
        {
            if (destroyed)
            {
                ++*destroyed;
            }
        }
    };
    {
        auto lease = std::shared_ptr<const void>(new int(1),
                                                 [&](const void *value)
                                                 {
                                                     assert(destroyed == 1);
                                                     ++code_released;
                                                     delete static_cast<const int *>(value);
                                                 });
        std::vector<DecodedComponent> abandoned;
        abandoned.push_back(DecodedComponent::own(Temporary(destroyed), std::move(lease)));
        assert(destroyed == 0 && code_released == 0);
        abandoned.clear();
        assert(destroyed == 1 && code_released == 1);
    }
    {
        int destroyed{}, code_released{};
        std::shared_ptr<const void> lease(new int(0), [&](const void *value)
        {
            assert(destroyed == 1);
            ++code_released;
            delete static_cast<const int *>(value);
        });
        std::vector<DecodedComponent> values;
        values.push_back(DecodedComponent::own(Temporary(destroyed), std::move(lease)));
        lux::process::TaskScope task;
        auto work = stdexec::then(stdexec::just_stopped(), [owned = std::move(values)]() noexcept
        {
            assert(false); // A stopped preparation must never enter installation.
        });
        assert(task.start(stdexec::upon_stopped(std::move(work), []() noexcept {})));
        assert(stdexec::sync_wait(task.close()));
        assert(destroyed == 1 && code_released == 1);
    }
    std::puts("C07 discarded and stopped TaskScope preparation: one destructor, then one provider-code lease release");
    std::puts("PASS owned component decode: no temporary Registry, malformed second value preserves destination, "
              "planned mutual references, null/existing/unresolved identities, new generation, generated custom schema "
              "and code lease");
}

int sceneWorkflow(const std::filesystem::path &, bool);

void pakRoundTrip(const std::filesystem::path &root)
{
    using namespace lux::asset;
    std::filesystem::create_directories(root);
    const auto id = [](std::string_view value) { return AssetId(*uuids::uuid::from_string(value)); };
    const auto first = id("10000000-0000-0000-0000-000000000001");
    const auto second = id("10000000-0000-0000-0000-000000000002");
    const auto third = id("10000000-0000-0000-0000-000000000003");
    auto bytes = std::make_shared<const std::string>("payload");
    const auto owned = lux::cxx::SharedBytes<>::fromOwner(bytes, std::as_bytes(std::span(*bytes)));
    std::vector<PakWriteEntry> entries{{first, 17, "Shared/Path", {}, owned},
                                       {second, 17, "Shared/Path", {}, {}, true},
                                       {third, 17, {}, {}, {}, true}};
    std::string error;
    const auto path = root / "tombstones.luxpak";
    assert(writePakFile(path, entries, "/Game", &error));
    std::ifstream file(path, std::ios::binary);
    auto image =
        std::make_shared<const std::string>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    auto decoded = decodePak(lux::cxx::SharedBytes<>::fromOwner(image, std::as_bytes(std::span(*image))), 3);
    if (!decoded)
    {
        std::printf("disk Pak failure: %s\n", decoded.error().c_str());
    }
    assert(decoded && decoded->entries.size() == 3 && !decoded->entries[0].metadata.tombstone);
    assert(decoded->entries[1].metadata.tombstone && decoded->entries[1].bytes.empty());
    assert(decoded->entries[2].metadata.tombstone && decoded->entries[2].metadata.vpath.empty());
    entries[1].source_bytes = owned;
    assert(!writePakFile(root / "invalid.luxpak", entries, "/Game", &error));
    assert(!std::filesystem::exists(root / "invalid.luxpak"));
    std::puts("PASS installed Pak: file tombstones, shared/empty virtual paths, exact payload rejection");
}

int main(int argc, char **argv)
{
    using namespace lux::simulation::ecs;
    const auto descriptors = consumer::schemas();
    assert(descriptors.size() == 2);
    const auto &schema = descriptors.front();
    assert(schema.editor_visible && schema.decode_emplace && schema.capture);
    assert(!descriptors.back().editor_visible && !descriptors.back().capture);
    decodedValues(schema);
    const auto binding = consumer::binding();
    assert(binding.type == schema.cpp_type && binding.draw);

    Registry registry;
    const auto entity = registry.create();
    registry.emplace<consumer::Component>(entity);
    WorldEntityMap identities;
    auto capture = schema.capture(registry, entity, {});
    assert(capture);
    auto encoded = capture->encode(identities, 1024 * 1024);
    assert(encoded);
    registry.get<consumer::Component>(entity).sequence.front().name = "edited after capture";
    const auto restored = registry.create();
    auto decoded = schema.decode_emplace(registry, identities, restored, 1, *encoded);
    assert(decoded);
    const auto &value = registry.get<consumer::Component>(restored);
    assert(value.sequence.front().name == "Unicode 中文");
    assert(value.flags.size() == 2 && value.flags[0] && !value.flags[1]);
    assert(value.map.at("key").size() == 2 && value.lookup.at(3) == "value");
    assert(lux::editor::scene::FieldValue<consumer::Component>::valid(value));
    assert(lux::editor::scene::FieldValue<consumer::Component>::equal(value, consumer::Component{}));

    std::puts("PASS installed component: separate domain/GUI DLLs, typed generated binding, nested codec and immutable "
              "capture");
    assert(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "cost"));
    const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::u8path(argv[1]) / run;
    pakRoundTrip(root / "pak");
    return sceneWorkflow(root / "scene", argc == 3);
}
