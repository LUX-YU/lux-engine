#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <algorithm>
#include <lux/engine/Engine.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/MeshQueryAssets.hpp>
#include <lux/engine/editor/ProjectAssets.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/process/assets/AssetRepository.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/scene/MeshQuerySystem.hpp>
#include <lux/engine/scene/SceneError.hpp>
#include <map>

namespace lux::editor
{
    struct MeshQueryAssets::Impl final
    {
        struct Version final
        {
            std::string package;
            std::uint64_t revision{};
            friend bool operator==(const Version&, const Version&) = default;
        };

        struct Geometry final
        {
            Version revision;
            std::shared_ptr<const scene::MeshQueryGeometry> value;
            std::optional<MeshQueryAssetFailure> failure;
        };

        struct Binding final
        {
            Impl& queries;
            scene::SceneInstanceId scene;
            // MeshQuerySystem owns this object until Scene participants have finished.
            // This is a callback subscription, not a retained Registry edit permission.
            scene::MeshQuery* query = nullptr;
        };

        struct Subscriber final
        {
            scene::SceneInstanceId scene;
            bool awaiting_geometry = false;
        };

        MeshQueryAssets& collection;
        EditorContext& context;
        ProjectAssets& catalog;
        execution::ExecutionRuntime& execution;
        execution::TaskScope& tasks;
        asset::AssetVfs& assets;
        std::map<asset::AssetId, Version> versions;
        std::map<asset::AssetId, Geometry> geometry;
        std::map<asset::AssetId, std::vector<Subscriber>> subscribers;
        std::map<scene::SceneInstanceId, std::unique_ptr<Binding>> bindings;
        object::Connection created, changed;

        Impl(MeshQueryAssets& collection, EditorContext& context, ProjectAssets& catalog) noexcept
            : collection(collection), context(context), catalog(catalog),
              execution(execution::ExecutionRuntime::instance()),
              tasks(context.tasks()), assets(context.assets())
        {
        }

        static Impl* resolve(object::ObjectId id) noexcept
        {
            auto object = object::ObjectRuntime::instance().resolve(id);
            return object ? static_cast<MeshQueryAssets*>(*object)->impl_.get() : nullptr;
        }

        void start(exec::task<void> work) noexcept
        {
            auto completed = execution::asResult(stdexec::starts_on(
                execution.mainScheduler(),
                stdexec::then(std::move(work), []() noexcept { return FrameworkResult<void>{}; })
            ));
            if (!execution.spawn(stdexec::then(
                    std::move(completed),
                    [](auto result) noexcept
                    {
                        if (!result)
                        {
                            std::terminate(); // Main admission remains open until project Scene cleanup finishes.
                        }
                    }
                )))
            {
                std::terminate();
            }
        }

        static exec::task<void> connectScene(
            object::ObjectId object,
            scene::SceneRuntime& runtime,
            scene::SceneInstanceId instance
        ) noexcept
        {
            auto connect = [object, instance](ecs::Registry& registry) noexcept
            {
                auto* state = resolve(object);
                if (!state)
                {
                    return;
                }
                auto found = state->bindings.find(instance);
                if (found == state->bindings.end())
                {
                    return;
                }
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    found->second->query = query;
                    query->setGeometryRequests(
                        found->second.get(),
                        +[](void* data, std::span<const asset::AssetId> meshes) noexcept
                        {
                            auto& binding = *static_cast<Binding*>(data);
                            binding.queries.request(binding.scene, meshes);
                        }
                    );
                }
            };
            static_cast<void>(co_await runtime.editInstanceAsync(instance, connect));
        }

        void sceneCreated(scene::SceneInstanceId instance) noexcept
        {
            auto system = context.engine().sceneRuntime().borrowSystem<scene::MeshQuerySystem>(instance);
            if (!system)
            {
                return;
            }
            auto binding = std::make_unique<Binding>(Binding{*this, instance});
            auto closed = context.engine().sceneRuntime().addCloseParticipant(
                instance,
                {{},
                 "editor.mesh_query.assets",
                 {},
                 this,
                 +[](void* value, scene::SceneInstanceId id) noexcept
                 { return static_cast<Impl*>(value)->closeScene(id); }}
            );
            if (!closed)
            {
                return; // Another publication listener may already have accepted Scene close.
            }
            bindings.emplace(instance, std::move(binding));
            start(connectScene(collection.objectId(), context.engine().sceneRuntime(), instance));
        }

        exec::task<scene::SceneCloseResult> closeScene(scene::SceneInstanceId instance) noexcept
        {
            auto found = bindings.find(instance);
            if (found == bindings.end())
            {
                co_return scene::SceneCloseResult{};
            }
            // Main dispatch cannot overlap SceneDriver/Simulation execution. Removing
            // our callback remains valid after Scene failure; author edits correctly do not.
            if (found->second->query)
            {
                found->second->query->setGeometryRequests(nullptr, nullptr);
            }
            for (auto entry = subscribers.begin(); entry != subscribers.end();)
            {
                std::erase_if(entry->second, [&](const Subscriber& value) { return value.scene == instance; });
                if (entry->second.empty())
                {
                    entry = subscribers.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
            bindings.erase(instance);
            co_return scene::SceneCloseResult{};
        }

        static exec::task<void> adopt(
            object::ObjectId object,
            scene::SceneRuntime& runtime,
            scene::SceneInstanceId instance,
            asset::AssetId mesh,
            Version revision,
            std::shared_ptr<const scene::MeshQueryGeometry> geometry
        ) noexcept
        {
            auto apply = [&](ecs::Registry& registry) noexcept
            {
                auto* state = resolve(object);
                if (!state)
                {
                    return;
                }
                auto binding = state->bindings.find(instance);
                auto current = state->versions.find(mesh);
                const bool stale =
                    binding == state->bindings.end() || current == state->versions.end() || current->second != revision;
                if (stale)
                {
                    return;
                }
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    if (geometry)
                    {
                        query->setGeometry(mesh, std::move(geometry));
                    }
                    else
                    {
                        query->setGeometryFailure(mesh, {scene::EMeshQueryError::ASSET_FAILURE, {}, mesh});
                    }
                }
            };
            static_cast<void>(co_await runtime.editInstanceAsync(instance, apply));
        }

        static exec::task<void> invalidate(
            object::ObjectId object,
            scene::SceneRuntime& runtime,
            scene::SceneInstanceId instance,
            std::vector<asset::AssetId> changed
        ) noexcept
        {
            auto apply = [&](ecs::Registry& registry) noexcept
            {
                auto* state = resolve(object);
                if (!state)
                {
                    return;
                }
                auto found = state->bindings.find(instance);
                if (found == state->bindings.end())
                {
                    return;
                }
                if (auto* query = registry.ctx().find<scene::MeshQuery>())
                {
                    for (auto mesh : changed)
                    {
                        query->removeGeometry(mesh);
                    }
                }
            };
            static_cast<void>(co_await runtime.editInstanceAsync(instance, apply));
        }

        void catalogChanged() noexcept
        {
            if (!catalog.ready())
            {
                return;
            }
            std::map<asset::AssetId, Version> next;
            for (const auto& record : catalog.records())
            {
                for (const auto& product : record.products)
                {
                    next.emplace(product.id, Version{record.package, record.revision});
                }
            }
            for (const auto& [mesh, version] : versions)
            {
                if (version.package.empty() && !next.contains(mesh))
                {
                    next.emplace(mesh, Version{{}, catalog.revision()});
                }
            }
            std::map<scene::SceneInstanceId, std::vector<asset::AssetId>> affected;
            for (auto entry = geometry.begin(); entry != geometry.end();)
            {
                const auto found = next.find(entry->first);
                if (found == next.end() || found->second != entry->second.revision)
                {
                    if (const auto changed = subscribers.find(entry->first); changed != subscribers.end())
                    {
                        for (auto& subscriber : changed->second)
                        {
                            subscriber.awaiting_geometry = false;
                            affected[subscriber.scene].push_back(entry->first);
                        }
                    }
                    entry = geometry.erase(entry);
                }
                else
                {
                    ++entry;
                }
            }
            versions = std::move(next);
            for (auto& [instance, meshes] : affected)
            {
                start(invalidate(collection.objectId(), context.engine().sceneRuntime(), instance, std::move(meshes)));
            }
        }

        void request(scene::SceneInstanceId instance, std::span<const asset::AssetId> meshes) noexcept
        {
            for (auto mesh : meshes)
            {
                auto version = versions.find(mesh);
                if (version == versions.end())
                {
                    version = versions.emplace(mesh, Version{{}, catalog.revision()}).first;
                }
                auto& users = subscribers[mesh];
                auto subscriber = std::ranges::find(users, instance, &Subscriber::scene);
                if (subscriber == users.end())
                {
                    users.push_back({instance});
                    subscriber = std::prev(users.end());
                }
                auto found = geometry.find(mesh);
                if (found != geometry.end() && (found->second.value || found->second.failure))
                {
                    subscriber->awaiting_geometry = false;
                    start(adopt(
                        collection.objectId(),
                        context.engine().sceneRuntime(),
                        instance,
                        mesh,
                        version->second,
                        found->second.value
                    ));
                    continue;
                }
                subscriber->awaiting_geometry = true;
                if (found != geometry.end())
                {
                    continue;
                }
                if (auto accepted = collection.prepare(mesh); !accepted)
                {
                    completeGeometry(mesh, version->second);
                }
            }
        }

        void completeGeometry(asset::AssetId mesh, const Version& revision) noexcept
        {
            const auto found = subscribers.find(mesh);
            if (found == subscribers.end())
            {
                return;
            }
            const auto& cached = geometry.at(mesh);
            for (auto& subscriber : found->second)
            {
                if (!subscriber.awaiting_geometry)
                {
                    continue;
                }
                subscriber.awaiting_geometry = false;
                const auto instance = subscriber.scene;
                start(adopt(
                    collection.objectId(),
                    context.engine().sceneRuntime(),
                    instance,
                    mesh,
                    revision,
                    cached.value
                ));
            }
        }

        using QueryGeometry = std::shared_ptr<const scene::MeshQueryGeometry>;
        using QueryCompletion = execution::ExecutionResult<QueryGeometry, MeshQueryAssetFailure>;

        static MeshQueryAssetResult<QueryGeometry> buildQueryGeometry(
            const rdesc::Mesh& mesh,
            std::stop_token stop
        ) noexcept
        {
            if (stop.stop_requested())
            {
                return cxx::unexpected(MeshQueryAssetFailure{EMeshQueryAssetError::CANCELLED, "Mesh query cancelled"});
            }
            auto built = scene::MeshQueryGeometry::build(mesh);
            if (!built)
            {
                return cxx::unexpected(MeshQueryAssetFailure{
                    EMeshQueryAssetError::BUILD,
                    "Mesh query geometry error " + std::to_string(static_cast<unsigned>(built.error().code))
                });
            }
            return std::move(*built);
        }

        static exec::task<process::assets::AssetResult<process::assets::AssetDataRef<rdesc::Mesh>>>
        readMesh(process::assets::AssetRepository& assets, asset::AssetId mesh) noexcept
        {
            auto use = assets.acquire<asset::MeshAsset>(mesh);
            if (!use)
            {
                co_return cxx::unexpected(use.error());
            }
            co_return co_await assets.read(*use);
        }

        static auto loadQueryGeometry(
            process::assets::AssetRepository& assets,
            execution::CpuScheduler cpu,
            asset::AssetId mesh,
            std::stop_token stop
        ) noexcept
        {
            auto loaded = stdexec::write_env(
                stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), readMesh(assets, mesh)),
                stdexec::prop{stdexec::get_stop_token, stop}
            );
            // Asset publication is on Main; derived BVH construction explicitly uses the CPU pool.
            return stdexec::then(
                stdexec::continues_on(execution::asResult(std::move(loaded)), cpu),
                [stop](auto loaded) noexcept -> MeshQueryAssetResult<QueryGeometry>
                {
                    if (!loaded)
                    {
                        return cxx::unexpected(MeshQueryAssetFailure{
                            EMeshQueryAssetError::READ, "Mesh asset loading failed"
                        });
                    }
                    return buildQueryGeometry(**loaded, stop);
                }
            );
        }

        static execution::ETaskState finishGeometry(
            object::ObjectId object,
            asset::AssetId mesh,
            Version revision,
            QueryCompletion result
        ) noexcept
        {
            const auto resolved = object::ObjectRuntime::instance().resolve(object);
            if (!resolved)
            {
                return execution::ETaskState::CANCELLED;
            }
            auto& collection = *static_cast<MeshQueryAssets*>(*resolved);
            auto& state = *collection.impl_;
            const auto found = state.geometry.find(mesh);
            if (found == state.geometry.end() || found->second.revision != revision)
            {
                return execution::ETaskState::CANCELLED;
            }
            if (result)
            {
                found->second.value = std::move(*result);
            }
            else
            {
                const auto* domain = result.error().domainFailure();
                found->second.failure = domain ? *domain
                                               : MeshQueryAssetFailure{
                                                     result.error().isCancelled() ? EMeshQueryAssetError::CANCELLED
                                                                                  : EMeshQueryAssetError::EXECUTION,
                                                     "Mesh query execution did not complete"
                                                 };
            }
            state.completeGeometry(mesh, revision);
            return result ? execution::ETaskState::SUCCEEDED : execution::ETaskState::FAILED;
        }

        MeshQueryAssetResult<void> admitGeometry(
            object::ObjectId object,
            asset::AssetId mesh,
            Version revision
        ) noexcept
        {
            auto& state = *this;
            auto reads = state.context.prepareAssets();
            if (!reads)
            {
                return cxx::unexpected(
                    MeshQueryAssetFailure{EMeshQueryAssetError::READ, "Project asset endpoint unavailable"}
                );
            }
            auto& target = state.geometry.find(mesh)->second;
            auto admitted = state.tasks.submit(
                {"Prepare mesh query", "Assets"},
                [object,
                 mesh,
                 revision,
                 assets = *reads,
                 cpu = state.execution.cpuScheduler(),
                 main = state.execution.mainScheduler()](execution::TaskReporter reporter) noexcept
                {
                    auto result =
                        execution::asResult(loadQueryGeometry(*assets, cpu, mesh, reporter.stopToken()));
                    auto delivered = stdexec::then(
                        stdexec::continues_on(std::move(result), main),
                        [object, mesh, revision](auto result) noexcept
                        { return finishGeometry(object, mesh, revision, std::move(result)); }
                    );
                    // Explicit stop governs the work; it must not cancel the final Main-thread status delivery.
                    return stdexec::write_env(
                        std::move(delivered),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    );
                }
            );
            if (!admitted)
            {
                if (admitted.error() == execution::EExecutionError::CAPACITY_EXCEEDED)
                {
                    state.awaitGeometryCapacity(object, mesh, revision);
                    return {}; // The accepted demand remains pending; capacity is not a content failure.
                }
                target.failure =
                    MeshQueryAssetFailure{EMeshQueryAssetError::EXECUTION, "Mesh query task admission failed"};
                return cxx::unexpected(*target.failure);
            }
            return {};
        }

        void awaitGeometryCapacity(object::ObjectId object, asset::AssetId mesh, Version revision) noexcept
        {
            auto capacity = stdexec::upon_stopped(
                stdexec::then(tasks.whenCapacityAvailable(), []() noexcept { return true; }),
                []() noexcept { return false; }
            );
            auto completion = stdexec::continues_on(std::move(capacity), execution.mainScheduler());
            // Only the existing ObjectId survives the unmonitored wait. Closing the TaskScope can
            // precede this final Main continuation: it never borrows the Context or ProjectAssets.
            auto accepted = execution.spawn(stdexec::then(
                std::move(completion),
                [object, mesh, revision](bool available) noexcept
                {
                    const auto resolved = object::ObjectRuntime::instance().resolve(object);
                    if (!resolved)
                    {
                        return;
                    }
                    auto& collection = *static_cast<MeshQueryAssets*>(*resolved);
                    auto& state = *collection.impl_;
                    const auto found = state.geometry.find(mesh);
                    if (found == state.geometry.end() || found->second.revision != revision)
                    {
                        return;
                    }
                    MeshQueryAssetResult<void> admitted =
                        available
                            ? state.admitGeometry(object, mesh, revision)
                            : cxx::unexpected(
                                  MeshQueryAssetFailure{EMeshQueryAssetError::CANCELLED, "Mesh query admission stopped"}
                              );
                    if (!admitted)
                    {
                        found->second.failure = admitted.error();
                        state.completeGeometry(mesh, revision);
                    }
                }
            ));
            if (!accepted)
            {
                std::terminate(); // Project tasks stop before shared ExecutionRuntime closes admission.
            }
        }
    };

    MeshQueryAssets::MeshQueryAssets(EditorContext& context, ProjectAssets& catalog) noexcept
        : impl_(std::make_unique<Impl>(*this, context, catalog))
    {
        auto created = connect(
            &context,
            &EditorContext::sceneAdded,
            this,
            [this](scene::SceneInstanceId id) noexcept { impl_->sceneCreated(id); },
            object::EDelivery::DIRECT
        );
        auto changed = connect(
            &catalog,
            &ProjectAssets::changed,
            this,
            [this]() noexcept { impl_->catalogChanged(); },
            object::EDelivery::DIRECT
        );
        if (!created || !changed)
        {
            std::terminate();
        }
        impl_->created = std::move(*created);
        impl_->changed = std::move(*changed);
        impl_->catalogChanged();
        for (auto id : context.scenes())
        {
            impl_->sceneCreated(id);
        }
    }

    MeshQueryAssetResult<void> MeshQueryAssets::prepare(asset::AssetId mesh) noexcept
    {
        auto& state = *impl_;
        auto version = state.versions.try_emplace(mesh, Impl::Version{{}, state.catalog.revision()}).first;
        auto found = state.geometry.find(mesh);
        if (found != state.geometry.end())
        {
            return found->second.failure ? MeshQueryAssetResult<void>{cxx::unexpected(*found->second.failure)}
                                         : MeshQueryAssetResult<void>{};
        }
        state.geometry.emplace(mesh, Impl::Geometry{version->second});
        auto accepted = state.admitGeometry(objectId(), mesh, version->second);
        if (!accepted)
        {
            state.geometry.at(mesh).failure = accepted.error();
        }
        return accepted;
    }

    std::shared_ptr<const scene::MeshQueryGeometry> MeshQueryAssets::geometry(asset::AssetId mesh) const noexcept
    {
        const auto found = impl_->geometry.find(mesh);
        return found == impl_->geometry.end() ? nullptr : found->second.value;
    }

    MeshQueryAssets::~MeshQueryAssets() noexcept
    {
        if (!impl_->bindings.empty())
        {
            std::terminate(); // Scene participants finish before project services.
        }
    }
} // namespace lux::editor
