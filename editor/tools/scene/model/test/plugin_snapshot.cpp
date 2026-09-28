#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include "../src/SceneSessionData.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
    struct Node final
    {
        int value{};
    };
    struct Payload final
    {
        std::shared_ptr<Node> node;
    };
    std::size_t deleted{};
    bool released_early{};
    std::size_t decoded_nodes{};
    using namespace lux;
    using namespace lux::editor;
    using namespace lux::editor::scene;
    namespace ecs = simulation::ecs;

    ecs::ComponentCapture capture(const void* raw, std::shared_ptr<const void> code)
    {
        struct Captured final
        {
            std::shared_ptr<const void> code;
            Payload value;
        };
        auto value = std::make_shared<const Captured>(Captured{std::move(code), *static_cast<const Payload*>(raw)});
        return {
            value,
            [](const void* raw, const ecs::WorldEntityMap&, std::size_t limit) -> ecs::ComponentEncodeResult {
                if (limit < sizeof(int))
                    return cxx::unexpected(
                        serialization::SerializationFailure{serialization::ESerializationError::INVALID_VALUE}
                    );
                const auto& value = static_cast<const Captured*>(raw)->value;
                std::vector<std::byte> bytes(sizeof(int));
                std::memcpy(bytes.data(), &value.node->value, sizeof(int));
                return bytes;
            }
        };
    }
    ecs::ComponentSchema schema(std::shared_ptr<const void> code)
    {
        return ecs::makeComponentSchema<Payload>(
            ecs::componentSchemaId("test.plugin.node"),
            1,
            ecs::EComponentSnapshotPolicy::COPY,
            std::move(code),
            nullptr,
            ecs::EComponentSemanticKind::DOMAIN_CONTRACT,
            [](const ecs::Registry& registry, ecs::Entity entity, std::shared_ptr<const void> code
            ) -> cxx::expected<ecs::ComponentCapture, ecs::ComponentDecodeFailure> {
                return capture(&registry.get<Payload>(entity), std::move(code));
            },
            [](std::uint32_t version,
               std::span<const std::byte> bytes,
               ecs::ComponentEntityResolver,
               std::shared_ptr<const void> code) -> ecs::ComponentDecodeResult {
                if (version != 1 || bytes.size() != sizeof(int))
                    return cxx::unexpected(ecs::ComponentDecodeFailure{});
                ++decoded_nodes;
                auto node = std::shared_ptr<Node>(new Node{}, [weak = std::weak_ptr<const void>(code)](Node* value) {
                    released_early |= weak.expired();
                    ++deleted;
                    delete value;
                });
                std::memcpy(&node->value, bytes.data(), sizeof(int));
                return ecs::DecodedComponent::own(Payload{std::move(node)}, std::move(code));
            },
            capture
        );
    }
}

void pluginSnapshot()
{
    std::optional<SceneSnapshot> snapshot;
    std::weak_ptr<const void> unloaded;
    {
        auto code = std::make_shared<int>(42);
        unloaded = code;
        std::vector values{schema(code)};
        for (const auto& component : ecs::transformComponentSchemas())
            if (component.cpp_type == cxx::typeToken<ecs::Transform3D>())
                values.push_back(component);
        auto metadata = ecs::ComponentSchemaSet::build(std::move(values));
        assert(metadata);
        const auto id = asset::AssetId{*uuids::uuid::from_string("11111111-2222-3333-4444-555555555555")};
        auto simulation = std::move(simulation::SimulationDescriptionBuilder{}).build();
        auto description = std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved();
        assert(simulation && description);
        const std::array ids{
            world::worldDataSchemaId("test.plugin.node"),
            world::worldDataSchemaId("lux.ecs.Transform3D")
        };
        auto package = lux::scene::createScenePackage(
            id,
            "plugin CPU",
            ids,
            std::make_shared<const simulation::SimulationDescription>(std::move(*simulation)),
            *description
        );
        assert(package);
        auto source = SceneSource::create(*package, *metadata);
        assert(source);
        auto session = SceneSession::create({17, 0, 1}, {}, std::move(*source));
        assert(session && (*session)->describe().dirty);
        const world::WorldObjectId object{id.uuid()};
        std::vector<std::byte> bytes(sizeof(int));
        const int value = 7;
        std::memcpy(bytes.data(), &value, sizeof(value));
        SceneEditBatch batch{(*session)->describe().current, "plugin node", {}};
        batch.edits.push_back(SceneCreateObject{{object, {0}, {{ecs::componentSchemaId("test.plugin.node"), 1, bytes}}}}
        );
        const world::WorldObjectId spatial{uuids::uuid_name_generator(id.uuid())("spatial")};
        auto transform = encodeSceneValue(ecs::Transform3D{}, *metadata, ecs::WorldEntityMap{}, 4096);
        assert(transform);
        batch.edits.push_back(SceneCreateObject{{spatial, {0}, {std::move(*transform)}}});
        assert((*session)->apply(std::move(batch)));
        auto frozen = (*session)->capture();
        assert(frozen);
        snapshot = std::move(*frozen);
        decoded_nodes = 0;
        SceneEditBatch field{(*session)->describe().current, "local field", {}};
        field.edits.push_back(SceneSetField::make<ecs::Transform3D>(
            {{(*session)->describe().id, (*session)->describe().current.state.history, spatial},
             ecs::componentSchemaId("lux.ecs.Transform3D"),
             "translation"},
            Eigen::Vector3d{9, 0, 0}
        ));
        assert((*session)->apply(std::move(field)));
        assert(decoded_nodes == 0); // Local fields must not rebuild unrelated plugin components.
        code.reset();
        assert(!unloaded.expired());
        // Deliberate private fault injection: the codec's captured value shares a mutable node.
        // A shallow ComponentCapture snapshot would change here; the public model exposes no such pointer.
        auto& live = detail::SceneSourceAccess::data(detail::SceneSessionAccess::data(**session).source);
        live.registry.get<Payload>(live.identities.entity(object)).node->value = 99;
        int captured{};
        std::memcpy(&captured, snapshot->objects().front().components.front().bytes.data(), sizeof(captured));
        assert(captured == 7);
        auto current = (*session)->capture();
        assert(current);
        std::memcpy(&captured, current->objects().front().components.front().bytes.data(), sizeof(captured));
        assert(captured == 99);
    }
    assert(deleted > 0 && !released_early && !unloaded.expired());
    snapshot.reset();
    assert(unloaded.expired() && !released_early);
    std::puts("X02-03/Q11 deep encoded snapshot isolates shared plugin node; deleters precede code release PASS");
}
