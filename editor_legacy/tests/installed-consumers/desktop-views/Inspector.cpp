#include "ObjectQueue.hpp"
#include <consumer/Component.ecs_schema.hpp>
#include <desktop_consumer.inspector.generated.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Root.hpp>
#include "ControlsTestAccess.hpp"
#include <cassert>
#include <cstdio>

namespace
{
    using namespace lux;
    using namespace lux::editor;
    namespace author = lux::editor::scene;
    template <class T> auto take(T result)
    {
        assert(result);
        return std::move(*result);
    }
}
int main()
{
    auto queue = take(object::ObjectMessageQueue::create(128));
    auto root = take(ui::Root::create(queue.dispatcherRef(), {.docking = false}));
    lux::test::ObjectQueue store_messages;
    sessions::SessionStore store{store_messages.dispatcherRef(), 4};
    const auto generated = simulation::ecs::generated::DesktopConsumerComponentSchemas();
    std::vector<simulation::ecs::ComponentSchema> copies;
    for (const auto& schema : generated)
        if (schema.snapshot == simulation::ecs::EComponentSnapshotPolicy::COPY)
            copies.push_back(schema);
    auto schemas = take(simulation::ecs::ComponentSchemaSet::build(std::move(copies)));
    const auto uuid = *uuids::uuid::from_string("98765432-1234-5678-1234-123456789abc");
    const std::array types{world::worldDataSchemaId("consumer.Component")};
    auto package = take(lux::scene::createScenePackage(
        asset::AssetId{uuid},
        "SDK Inspector",
        types,
        std::make_shared<const simulation::SimulationDescription>(
            take(std::move(simulation::SimulationDescriptionBuilder{}).build())
        ),
        take(std::move(lux::scene::SceneDescriptionBuilder{}).buildResolved())
    ));
    auto reservation = take(store.reserve<author::SceneSession>({"lux.editor.scene"}, lux::object::CodeLease::builtin()));
    auto model = take(author::SceneSession::create(
        reservation.id(),
        std::nullopt,
        take(author::SceneSource::create(package, schemas))
    ));
    auto* session = model.get();
    assert(store.prepare(reservation, model));
    const auto key = take(store.key<author::SceneSession>(take(store.publish(reservation))));
    simulation::ecs::WorldEntityMap identities;
    author::SceneEditBatch seed{session->describe().current, "SDK seed", {}};
    seed.edits.emplace_back(author::SceneCreateObject{
        {world::WorldObjectId{uuid},
         {0},
         {take(author::encodeSceneValue(consumer::Component{}, schemas, identities, 65536))}}
    });
    assert(session->apply(std::move(seed)));
    const auto initial = session->describe();
    author::SceneInteractionGroup interaction(store.access<author::SceneSession>(), key, {1});
    const author::SceneObjectRef target{key.id(), initial.current.state.history, world::WorldObjectId{uuid}};
    const auto bindings = author::generated::desktop_consumerBindings();
    auto candidate = std::make_unique<author::InspectorView>(
        queue.dispatcherRef(),
        ui::PaneId{"sdk-fields"},
        store.access<author::SceneSession>(),
        schemas,
        std::vector<author::InspectorComponent>{bindings.begin(), bindings.end()}
    );
    auto* inspector = candidate.get();
    assert(inspector->status() && inspector->rebind({key, &interaction}, target));
    assert(!inspector->attachedRoot());
    assert(root->addSubPane(std::move(candidate)) && !candidate);
    const auto id = take(root->identify(*inspector));
    const auto frame = [&] { assert(root->update({{900, 800}, .016F}, nullptr)); };
    frame();
    const auto find = [](auto&& self, object::LuxObject& object) -> ui::NumericEdit* {
        if (auto* numeric = dynamic_cast<ui::NumericEdit*>(&object))
            return numeric;
        for (auto* child = object.firstChild(); child; child = child->nextSibling())
            if (auto* result = self(self, *child))
                return result;
        return nullptr;
    };
    auto* number = find(find, *inspector);
    assert(number && std::get<double>(number->value()) == 1.5);
    number->setValue(3.0);
    static_cast<void>(ui::ControlsTestAccess::edited(*number, {true, true, false, false}));
    frame();
    assert(interaction.overlay() && session->describe().current == initial.current);
    assert(inspector->finishEditing());
    frame();
    assert(session->describe().current != initial.current);
    assert(session->undo());
    frame();
    assert(std::get<double>(number->value()) == 1.5 && session->describe().current == initial.current);
    assert(session->redo());
    frame();
    assert(std::get<double>(number->value()) == 3.0);
    const auto findElement = [](auto&& self, object::LuxObject& object, std::string_view identity) -> ui::Element* {
        if (auto* element = dynamic_cast<ui::Element*>(&object); element && element->id().name() == identity)
            return element;
        for (auto* child = object.firstChild(); child; child = child->nextSibling())
            if (auto* result = self(self, *child, identity))
                return result;
        return nullptr;
    };
    auto* values = findElement(findElement, *inspector, "consumer::Component::values");
    assert(values);
    auto* add = dynamic_cast<ui::Button*>(findElement(findElement, *values, "add"));
    assert(add);
    const auto before_set = session->describe().current;
    static_cast<void>(ui::ControlsTestAccess::activate(*add));
    assert(session->describe().current == before_set);
    frame();
    assert(inspector->status() && session->describe().current != before_set);
    consumer::Component expected;
    expected.settings.gain = 3.0;
    expected.values.insert(0);
    auto encoded = take(author::encodeSceneValue(expected, schemas, identities, 65536));
    auto actual = take(take(session->read()).component(target, encoded.schema));
    assert(actual.bytes == encoded.bytes);
    assert(session->undo());
    frame();
    assert(session->describe().current == before_set);
    assert(session->redo());
    frame();
    assert(take(take(session->read()).component(target, encoded.schema)).bytes == encoded.bytes);
    const auto final_content = session->describe().current;
    assert(inspector->prepareClose());
    assert(root->removeSubPane(*inspector));
    assert(!root->findPane(id));
    (void)queue.collectRetired();
    assert(store.describe(key.id()) && session->describe().current == final_content);
    std::puts("PASS SDK generated nested/container Inspector Elements, scalar preview/commit, set insertion, "
              "exact author encoding, Undo/Redo, close without legacy UI");
}
