#include "ObjectQueue.hpp"
#include "../src/SceneSessionData.hpp"
#include <lux/engine/editor/scene/SceneSession.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/editor/scene/PreparedSceneReload.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <functional>
#include <stdexcept>
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
    std::function<void()> encoding_hook;
    bool encode_error{};
    std::function<void()> capture_release_hook;
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
            Captured(std::shared_ptr<const void> lease, Payload payload)
                : code(std::move(lease)), value(std::move(payload))
            {}
            ~Captured()
            {
                if (auto hook = std::exchange(capture_release_hook, {}))
                    hook();
            }
        };
        auto value = std::make_shared<const Captured>(std::move(code), *static_cast<const Payload*>(raw));
        return {
            value,
            [](const void* raw, const ecs::WorldEntityMap&, std::size_t limit) -> ecs::ComponentEncodeResult {
                if (auto hook = std::exchange(encoding_hook, {}))
                    hook();
                if (encode_error || limit < sizeof(int))
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

void codecReadRegression(int scenario)
{
    const auto take = []<class T>(T result) {
        assert(result);
        return std::move(*result);
    };
    std::vector values{schema(std::make_shared<int>(42))};
    for (const auto& component : ecs::transformComponentSchemas())
        if (component.cpp_type == cxx::typeToken<ecs::Transform3D>())
            values.push_back(component);
    auto metadata = take(ecs::ComponentSchemaSet::build(std::move(values)));
    const asset::AssetId root{*uuids::uuid::from_string("11111111-2222-3333-4444-555555555555")};
    const std::array ids{world::worldDataSchemaId("test.plugin.node"), world::worldDataSchemaId("lux.ecs.Transform3D")};
    auto simulation = take(std::move(simulation::SimulationDescriptionBuilder{}).build());
    auto description = take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved());
    auto package = take(lux::scene::createScenePackage(
        root,
        "codec reentry",
        ids,
        std::make_shared<const simulation::SimulationDescription>(std::move(simulation)),
        description
    ));
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 2};
    auto reservation = take(store.reserve<SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
    auto candidate = take(SceneSession::create(
        reservation.id(),
        sessions::BoundSource{root, "codec.scene"},
        take(SceneSource::create(package, metadata))
    ));
    auto& session = *candidate;
    assert(store.prepare(reservation, candidate) && store.publish(reservation));
    const world::WorldObjectId plugin{root.uuid()};
    const world::WorldObjectId spatial{uuids::uuid_name_generator(root.uuid())("spatial")};
    const auto plugin_schema = ecs::componentSchemaId("test.plugin.node");
    const auto transform_schema = ecs::componentSchemaId("lux.ecs.Transform3D");
    std::vector<std::byte> bytes(sizeof(int));
    const int value = 7;
    std::memcpy(bytes.data(), &value, sizeof(value));
    SceneEditBatch create{session.describe().current, "create codec test", {}};
    create.edits.push_back(SceneCreateObject{{plugin, {0}, {{plugin_schema, 1, bytes}}}});
    create.edits.push_back(SceneCreateObject{
        {spatial, {0}, {take(encodeSceneValue(ecs::Transform3D{}, metadata, ecs::WorldEntityMap{}, 4096))}}
    });
    assert(session.apply(std::move(create)));
    const auto baseline = session.describe();
    const auto frozen = take(session.capture());
    const auto ref = [&](world::WorldObjectId id) {
        return SceneObjectRef{baseline.id, baseline.current.state.history, id};
    };
    const auto edit = [&] {
        SceneEditBatch batch{session.describe().current, "nested field", {}};
        batch.edits.push_back(SceneSetField::make<ecs::Transform3D>(
            {ref(spatial), transform_schema, "translation"},
            Eigen::Vector3d{9, 8, 7}
        ));
        return session.apply(std::move(batch));
    };
    const auto busy = [](const auto& result) {
        return !result && result.error().code == ESceneEditError::SESSION &&
               result.error().session == sessions::ESessionError::BUSY;
    };
    const auto unchanged = [&] {
        const auto info = session.describe();
        assert(info.current == baseline.current && info.observed == baseline.observed && info.dirty == baseline.dirty);
        assert(info.admission == sessions::EEditAdmission::AVAILABLE);
        const auto current = take(session.capture());
        assert(current.cursor() == frozen.cursor());
        assert(std::ranges::equal(current.objects(), frozen.objects()));
    };
    if (scenario == 5 || scenario == 8)
    {
        bool rejected{};
        encoding_hook = [&] {
            auto nested = edit();
            rejected = busy(nested);
            std::printf(
                "R02-0%d nested_apply=%d error=%u session_error=%u\n",
                scenario,
                bool(nested),
                nested ? 0u : static_cast<unsigned>(nested.error().code),
                nested ? 0u : static_cast<unsigned>(nested.error().session)
            );
        };
        if (scenario == 5)
        {
            auto snapshot = take(session.capture());
            std::printf(
                "snapshot_stamp_unchanged=%d live_stamp_unchanged=%d snapshot_payload_unchanged=%d\n",
                snapshot.content() == baseline.current,
                session.describe().current == baseline.current,
                std::ranges::equal(snapshot.objects(), frozen.objects())
            );
            std::fflush(stdout);
            assert(rejected);
            assert(snapshot.content() == baseline.current && snapshot.cursor() == frozen.cursor());
            assert(std::ranges::equal(snapshot.objects(), frozen.objects()));
        }
        else
        {
            const auto component = take(take(session.read()).component(ref(plugin), plugin_schema));
            std::fflush(stdout);
            assert(rejected && component.bytes == bytes);
        }
        unchanged();
        bool release_rejected{};
        capture_release_hook = [&] { release_rejected = busy(edit()); };
        assert(session.capture());
        assert(release_rejected);
        unchanged();
        assert(edit());
    }
    else if (scenario == 6)
    {
        encode_error = true;
        auto failure = session.capture();
        auto component_failure = take(session.read()).component(ref(plugin), plugin_schema);
        encode_error = false;
        assert(!failure && failure.error().code == ESceneEditError::CODEC);
        assert(!component_failure && component_failure.error().code == ESceneEditError::CODEC);
        unchanged();
        encoding_hook = [] { throw std::runtime_error("test codec"); };
        bool caught{};
        try
        {
            (void)session.capture();
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        assert(caught);
        unchanged();
        encoding_hook = [] { throw std::runtime_error("test component codec"); };
        caught = false;
        try
        {
            (void)take(session.read()).component(ref(plugin), plugin_schema);
        }
        catch (const std::runtime_error&)
        {
            caught = true;
        }
        assert(caught);
        unchanged();
        assert(edit());
    }
    else if (scenario == 7)
    {
        auto reload = take(PreparedSceneReload::prepare(session, take(SceneSource::create(package, metadata))));
        encoding_hook = [&] {
            auto close = store.prepareClose(baseline.current);
            assert(!close && close.error() == sessions::ESessionError::BUSY);
            auto& state = detail::SceneSessionAccess::data(session).state;
            auto binding = state.prepareBindingChange(baseline.current, baseline.current);
            assert(!binding && binding.error() == sessions::ESessionError::BUSY);
            assert(busy(session.undo()) && busy(session.redo()));
            assert(busy(reload.adopt(session)));
        };
        assert(session.capture());
        unchanged();
        const auto borrowed = take(session.read());
        {
            auto close = take(store.prepareClose(baseline.current));
            assert(!session.capture());
            assert(busy(borrowed.component(ref(plugin), plugin_schema)));
        }
        assert(reload.adopt(session));
        assert(session.describe().current.state.history != baseline.current.state.history);
    }
    std::printf("R02-0%d codec read regression PASS\n", scenario);
}
