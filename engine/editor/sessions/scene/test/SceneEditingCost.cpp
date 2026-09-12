#include <lux/engine/editor/sessions/scene/SceneEditInput.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>
namespace
{
    using namespace lux::editor;
    namespace ecs = lux::simulation::ecs;
    template<class T> T identity(std::uint8_t number)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = number;
        return T{uuids::uuid{bytes}};
    }
    sessions::SceneEditInput source(lux::object::ObjectDispatcherRef dispatcher, std::uint64_t session)
    {
        auto components = ecs::ComponentSchemaSet::build(std::vector{
            ecs::makeComponentSchema<ecs::Transform3D>(ecs::componentSchemaId("lux.ecs.Transform3D"), 1,
                ecs::EComponentSnapshotPolicy::COPY, {}, nullptr, ecs::EComponentSemanticKind::FOUNDATION, true),
            ecs::makeComponentSchema<ecs::Light3D>(ecs::componentSchemaId("lux.ecs.Light3D"), 2,
                ecs::EComponentSnapshotPolicy::COPY, {}, nullptr, ecs::EComponentSemanticKind::DOMAIN_CONTRACT, true)
        });
        assert(components);
        auto metadata = lux::scene::SceneMetaManager::build({std::move(*components), {}, {}, {}, {}});
        assert(metadata);
        auto shared = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
        lux::world::WorldDescriptionBuilder wb;
        assert(wb.setIdentity(identity<lux::world::WorldBundleId>(1),
            identity<lux::world::WorldBundleGeneration>(1), "ER2 finite author input"));
        assert(wb.setPartitioner({lux::world::worldPartitionerId("test.none"), 1}, 0));
        auto world = std::move(wb).build();
        lux::simulation::SimulationDescriptionBuilder sb;
        auto simulation = std::move(sb).build();
        lux::scene::SceneDescriptionBuilder cb;
        cb.setWorld(identity<lux::asset::AssetId>(1));
        cb.setSimulation(identity<lux::asset::AssetId>(2));
        auto description = std::move(cb).build();
        assert(world && simulation && description);
        auto scene = lux::scene::Scene::create({
            std::make_shared<lux::scene::SceneDescription>(std::move(*description)),
            std::make_shared<lux::world::WorldDescription>(std::move(*world)),
            std::make_shared<lux::simulation::SimulationDescription>(std::move(*simulation)), *shared, {}});
        assert(scene && (*scene)->simulation().seal());
        const auto entity = (*scene)->registry().create();
        const ecs::Transform3D transform;
        const ecs::Light3D light;
        (*scene)->registry().emplace<ecs::Transform3D>(entity, transform);
        (*scene)->registry().emplace<ecs::Light3D>(entity, light);
        sessions::SceneEditInput result;
        result.source.id = {session};
        result.source.dispatcher = std::move(dispatcher);
        result.source.scene = std::move(*scene);
        result.source.metadata = std::move(shared);
        result.source.initial_selection = entity;
        result.source.history_limits = {16, 1024 * 1024, 64 * 1024, 128};
        result.objects.push_back({identity<lux::world::WorldObjectId>(1), entity, transform, light});
        return result;
    }
    void close(std::unique_ptr<sessions::SceneSession> &session)
    {
        assert(session->beginClose());
        const auto result = session->advanceClose();
        assert(result && *result == sessions::ECloseProgress::COMPLETE);
        session.reset();
    }
    struct Observer final : lux::object::Object<Observer>
    {
        using Object::Object;
        sessions::SceneSession *session{};
        sessions::SceneObjectRef target;
        std::size_t calls{};
        void committed(const sessions::SceneContentNotice &notice) noexcept
        {
            ++calls;
            const auto history = session->historyView();
            const auto author = session->readAuthor(target);
            assert(history && author && history->history.revision.value == notice.stamp.content_revision);
            const auto reentrant = session->beginClose();
            assert(!reentrant && reentrant.error().code == sessions::ESceneError::BUSY);
        }
    };
}

#include <chrono>
#include <charconv>
int main(int argc, char **argv)
{
    using namespace lux::editor;
    assert(argc == 2);
    std::size_t count{};
    const std::string_view text{argv[1]};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
    assert(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size());
    assert(count == 1000 || count == 10000);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::object::ObjectMessageQueue queue;
        auto run = [&](std::size_t size, bool measured) {
            auto input = source(queue.dispatcherRef(), measured ? 2 : 1);
            input.source.history_limits = {size, 64 * 1024 * 1024, 64 * 1024, 128};
            const sessions::SceneObjectRef target{input.source.id, input.objects.front().object};
            auto opened = sessions::SceneSession::openEditing(input);
            assert(opened);
            auto session = std::move(*opened);
            Observer observer(queue.dispatcherRef());
            observer.session = session.get();
            observer.target = target;
            auto connection = session->observe<sessions::SceneSession::contentChanged,
                &Observer::committed, lux::object::EDelivery::DIRECT>(observer);
            assert(connection);
            std::uint64_t cycle{}, checksum{};
            const auto project = [&] {
                const sessions::SceneOwnerUpdate update{++cycle, 0};
                assert(session->updateAtOwnerSafePoint(update) && session->advanceScene(update));
            };
            project();
            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            for (std::size_t i = 1; i <= size; ++i)
            {
                const auto token = session->beginTransformEdit(target);
                assert(token);
                ecs::Transform3D value;
                for (unsigned preview = 1; preview <= 100; ++preview)
                {
                    value.translation.x() = double(i) + (double(preview) - 100) / 100;
                    assert(session->previewTransform(*token, value));
                }
                assert(session->commitTransformEdit(*token));
                project();
                const auto actual = session->readAuthor(target)->transform->translation.x();
                assert(actual == double(i));
                checksum += static_cast<std::uint64_t>(actual);
            }
            const auto executed = Clock::now();
            for (std::size_t i = size; i > 0; --i)
            {
                assert(session->undo()); project();
                assert(session->readAuthor(target)->transform->translation.x() == double(i - 1));
                checksum += i - 1;
            }
            const auto undone = Clock::now();
            for (std::size_t i = 1; i <= size; ++i)
            {
                assert(session->redo()); project();
                assert(session->readAuthor(target)->transform->translation.x() == double(i));
                checksum += i;
            }
            const auto redone = Clock::now();
            const auto accounting = session->historyView()->history;
            assert(observer.calls == 3 * size && accounting.entry_count == size);
            assert(checksum == (3 * size * size + size) / 2);
            close(session);
            const auto closed = Clock::now();
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            if (measured)
                std::printf("ER2_COST gestures=%zu previews=%zu undo=%zu redo=%zu notifications=%zu checksum=%llu "
                    "execute_project_ms=%.6f undo_project_ms=%.6f redo_project_ms=%.6f close_ms=%.6f "
                    "metadata=%zu retained=%zu staging_limit=65536 author_objects=1 gpu_views=0 warmup=100\n",
                    size, size * 100, size, size, observer.calls, checksum,
                    ms(start, executed), ms(executed, undone), ms(undone, redone), ms(redone, closed),
                    accounting.history_metadata_bytes, accounting.charged_retained_bytes);
        };
        run(100, false);
        run(count, true);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
