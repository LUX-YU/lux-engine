#include <lux/engine/EngineContext.hpp>
#include <lux/engine/process/CompletionWork.hpp>
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/scene/SceneRuntime.hpp>
#include <lux/engine/scene/ScenePackage.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/process/world_loading/WorldMemoryStorageSource.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <cassert>
#include <chrono>
#include <latch>
#include <cstdio>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
    void taskChecks(lux::process::ExecutionRuntime& runtime)
    {
        using namespace lux::process;
        enum class EFailure
        {
            REJECTED
        };
        using Value = lux::cxx::expected<std::unique_ptr<int>, EFailure>;
        using Result = TTaskResult<std::unique_ptr<int>, EFailure>;
        unsigned calls{};
        Task nested;
        auto task = runtime.submit(
            {"immediate", "test"},
            [](TaskReporter reporter) noexcept {
                reporter.setPhase("ready");
                reporter.setProgress(1, 1);
                return stdexec::just(Value{std::make_unique<int>(73)});
            },
            [&](Result&& result) noexcept {
                assert(result && **result == 73);
                ++calls;
                auto created = runtime.submit(
                    {"nested", "test"},
                    [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
                    [&](TTaskResult<void, EFailure>&& value) noexcept {
                        assert(value);
                        ++calls;
                    }
                );
                assert(created);
                nested = std::move(*created);
            }
        );
        assert(task && calls == 0);
        const auto old_id = task->id();
        assert(runtime.taskInfo(old_id)->progress->completed == 1);
        assert(*runtime.collectCompletions() == 1 && calls == 0);
        assert(*runtime.dispatchTaskEvents() == 1 && calls == 1);
        assert(*runtime.collectCompletions() == 1);
        assert(*runtime.dispatchTaskEvents() == 1 && calls == 2);
        *task = {};
        nested = {};

        const auto owner_thread = std::this_thread::get_id();
        bool on_worker{};
        bool adopted{};
        auto cpu = runtime.submit(
            {"cpu", "test"},
            [&](TaskReporter) noexcept {
                return stdexec::then(stdexec::schedule(runtime.cpu()), [&]() noexcept -> Value {
                    on_worker = std::this_thread::get_id() != owner_thread;
                    return lux::cxx::unexpected(EFailure::REJECTED);
                });
            },
            [&](Result&& result) noexcept {
                assert(std::this_thread::get_id() == owner_thread && on_worker);
                assert(!result && *result.error().domainFailure() == EFailure::REJECTED);
                adopted = true;
            }
        );
        assert(cpu && cpu->id() != old_id);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!adopted)
        {
            const auto epoch = runtime.wakeEpoch();
            assert(runtime.collectCompletions());
            assert(runtime.dispatchTaskEvents());
            if (!adopted)
                runtime.waitForWork(epoch, deadline);
            assert(std::chrono::steady_clock::now() < deadline);
        }
        *cpu = {};

        std::latch entered{1};
        bool worker_exited{};
        {
            auto cancelled = runtime.submit(
                {"cancel", "test"},
                [&](TaskReporter reporter) noexcept {
                    return stdexec::then(stdexec::schedule(runtime.cpu()), [&, reporter]() noexcept {
                        const auto stop = reporter.stopToken();
                        std::atomic<bool> signalled{};
                        std::stop_callback notify(stop, [&]() noexcept {
                            signalled.store(true, std::memory_order_release);
                            signalled.notify_one();
                        });
                        entered.count_down();
                        signalled.wait(false, std::memory_order_acquire);
                        worker_exited = true;
                        return lux::cxx::expected<void, EFailure>{};
                    });
                },
                [&](TTaskResult<void, EFailure>&&) noexcept { ++calls; }
            );
            assert(cancelled);
            entered.wait();
        }
        assert(worker_exited && calls == 2); // Destruction settled work without business reentry.
        assert(runtime.dispatchTaskEvents() && calls == 2);

        unsigned destroyed{};
        auto pin = std::make_shared<int>(1);
        struct Capture final
        {
            std::weak_ptr<int> code;
            unsigned& destroyed;
            ~Capture()
            {
                assert(!code.expired());
                ++destroyed;
            }
        };
        auto pinned = runtime.submit(
            {"code", "test", {}, pin},
            [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
            [capture =
                 std::make_unique<Capture>(std::weak_ptr<int>{pin}, destroyed)](TTaskResult<void, EFailure>&& result
            ) noexcept { assert(result); }
        );
        pin.reset();
        assert(pinned);
        *pinned = {};
        assert(destroyed == 1);
        assert(runtime.dispatchTaskEvents());

        // Scoped completion is storage/transport work: collection may settle it without business dispatch.
        unsigned settled{};
        {
            TaskScope services(runtime);
            assert(services.submit(
                {"settle storage", "test"},
                [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
                [&](TTaskResult<void, EFailure>&& result) noexcept {
                    assert(result);
                    ++settled;
                }
            ));
            assert(settled == 0);
            assert(services.join());
            assert(settled == 1 && calls == 2);
        }

        auto small = ExecutionRuntime::create({1, 4, 1, {4}, {}, 1});
        assert(small);
        auto first = small->submit(
            {"first", "test"},
            [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
            [](TTaskResult<void, EFailure>&&) noexcept {}
        );
        assert(first);
        const auto first_id = first->id();
        auto full = small->submit(
            {},
            [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
            [](TTaskResult<void, EFailure>&&) noexcept {}
        );
        assert(!full && full.error() == EExecutionError::CAPACITY_EXCEEDED);
        *first = {};
        auto reused = small->submit(
            {"second", "test"},
            [](TaskReporter) noexcept { return stdexec::just(lux::cxx::expected<void, EFailure>{}); },
            [](TTaskResult<void, EFailure>&&) noexcept {}
        );
        assert(reused && reused->id().slot.index == first_id.slot.index && reused->id() != first_id);
        *reused = {};
        assert(!small->taskInfo(first_id) && small->taskInfos().size() == 1);
    }

    void taskMeasurements(lux::process::ExecutionRuntime& runtime)
    {
        using namespace lux::process;
        using Clock = std::chrono::steady_clock;
        struct Value final
        {
            unsigned* moves;
            explicit Value(unsigned& count) noexcept : moves(&count) {}
            Value(Value&& other) noexcept : moves(other.moves)
            {
                ++*moves;
            }
            Value(const Value&) = delete;
        };
        using Result = lux::cxx::expected<Value, EExecutionError>;
        std::chrono::nanoseconds admission{}, collection{}, delivery{}, cold{};
        unsigned moves{};
        constexpr unsigned warmup = 32, samples = 512;
        for (unsigned sample{}; sample < warmup + samples; ++sample)
        {
            const auto begin = Clock::now();
            auto task = runtime.submit(
                {"measure", "test"},
                [&](TaskReporter) noexcept { return stdexec::just(Result{Value{moves}}); },
                [](TTaskResult<Value, EExecutionError>&& value) noexcept { assert(value); }
            );
            const auto submitted = Clock::now();
            assert(task && runtime.collectCompletions());
            const auto collected = Clock::now();
            assert(runtime.dispatchTaskEvents());
            const auto delivered = Clock::now();
            if (sample == 0)
                cold = delivered - begin;
            if (sample >= warmup)
            {
                admission += submitted - begin;
                collection += collected - submitted;
                delivery += delivered - collected;
            }
        }
        std::printf(
            "TASK BASELINE samples=%u cold_ns=%lld admission_ns=%lld collect_ns=%lld dispatch_ns=%lld "
            "moves_per_value=%u copies=0\n",
            samples,
            static_cast<long long>(cold.count()),
            static_cast<long long>(admission.count() / samples),
            static_cast<long long>(collection.count() / samples),
            static_cast<long long>(delivery.count() / samples),
            moves / (warmup + samples)
        );
    }

#if defined(_WIN32)
    // Test-only snapshots: count live CRT heap blocks, not every transient allocation in the process.
    struct HeapSample final
    {
        long long blocks{}, bytes{};
    };
    HeapSample heapSample()
    {
        HeapSample sample;
        const auto heap = GetProcessHeap();
        assert(HeapLock(heap));
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap, &entry))
            if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0)
            {
                ++sample.blocks;
                sample.bytes += entry.cbData;
            }
        const auto error = GetLastError();
        assert(HeapUnlock(heap));
        assert(error == ERROR_NO_MORE_ITEMS);
        return sample;
    }
    unsigned long long cpuTicks()
    {
        FILETIME created{}, exited{}, kernel{}, user{};
        assert(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
        const auto value = [](FILETIME time) {
            return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return value(kernel) + value(user);
    }
    void taskMemoryAndWait(lux::process::ExecutionRuntime& runtime)
    {
        using namespace lux::process;
        using Value = lux::cxx::expected<void, EExecutionError>;
        assert(runtime.collectCompletions() && runtime.dispatchTaskEvents());
        const auto before = heapSample();
        auto task = runtime.submit(
            {"heap", "test"},
            [](TaskReporter) noexcept { return stdexec::just(Value{}); },
            [](TTaskResult<void, EExecutionError>&&) noexcept {}
        );
        assert(task);
        const auto admitted = heapSample();
        assert(runtime.collectCompletions());
        const auto collected = heapSample();
        assert(runtime.dispatchTaskEvents());
        const auto dispatched = heapSample();
        *task = {};
        const auto released = heapSample();
        std::printf(
            "TASK HEAP live admission_blocks=%lld admission_bytes=%lld collection_blocks=%lld "
            "collection_bytes=%lld dispatch_blocks=%lld release_total_blocks=%lld release_total_bytes=%lld\n",
            admitted.blocks - before.blocks,
            admitted.bytes - before.bytes,
            collected.blocks - admitted.blocks,
            collected.bytes - admitted.bytes,
            dispatched.blocks - collected.blocks,
            released.blocks - before.blocks,
            released.bytes - before.bytes
        );

        bool completed{};
        TaskScope timer(runtime);
        using TimerResult = lux::cxx::expected<void, ETimerError>;
        assert(timer.submit(
            {"idle wait", "test"},
            [&](TaskReporter) noexcept {
                return stdexec::upon_error(
                    stdexec::then(
                        runtime.timer().after(std::chrono::milliseconds(50)),
                        []() noexcept { return TimerResult{}; }
                    ),
                    [](ETimerError error) noexcept -> TimerResult { return lux::cxx::unexpected(error); }
                );
            },
            [&](TTaskResult<void, ETimerError>&& result) noexcept {
                assert(result);
                completed = true;
            }
        ));
        const auto cpu = cpuTicks();
        const auto begin = std::chrono::steady_clock::now();
        assert(runtime.waitUntil([&] { return completed; }));
        const auto wall = std::chrono::steady_clock::now() - begin;
        std::printf(
            "TASK WAIT wall_us=%lld cpu_us=%llu\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(wall).count()),
            (cpuTicks() - cpu) / 10
        );
    }
#endif

    void executionChecks(lux::process::ExecutionRuntime& runtime)
    {
        using namespace lux::process;
        struct Owner final
        {
            unsigned calls{};
            CompletionWork::Request next;
            static void run(void* pointer) noexcept
            {
                auto& owner = *static_cast<Owner*>(pointer);
                ++owner.calls;
                owner.next.request();
            }
        } a, b;
        CompletionWork first(runtime, &a, &Owner::run), second(runtime, &b, &Owner::run);
        a.next = second.requester();
        first.request();
        first.request();
        assert(*runtime.collectCompletions() == 1 && a.calls == 1 && b.calls == 0);
        assert(runtime.hasPendingWork());
        assert(*runtime.collectCompletions() == 1 && b.calls == 1);
        second.request();
        second.cancel();
        assert(*runtime.collectCompletions() == 0);
        CompletionWork::Request late;
        {
            CompletionWork temporary(runtime, &b, &Owner::run);
            late = temporary.requester();
            temporary.request();
        }
        late.request();
        assert(*runtime.collectCompletions() == 0 && b.calls == 1);
        std::atomic<bool> stop{};
        std::atomic<bool> started{};
        std::jthread producer([request = first.requester(), &stop, &started] {
            started.store(true, std::memory_order_release);
            while (!stop.load(std::memory_order_acquire))
                request.request();
        });
        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        first.cancel();
        stop.store(true, std::memory_order_release);
        producer.join();
        assert(*runtime.collectCompletions() == 0);

        std::atomic<unsigned> stages{};
        TaskScope tasks(runtime);
        assert(tasks.submit({"service", "test"}, [&](TaskReporter) noexcept {
            return stdexec::upon_error(
                stdexec::then(stdexec::schedule(runtime.cpu()), [&]() noexcept { ++stages; }),
                [](EExecutionError) noexcept { std::terminate(); }
            );
        }));
        std::jthread wrong_thread([&] {
            const auto result = tasks.join();
            assert(!result && result.error() == EExecutionError::WRONG_THREAD);
        });
        wrong_thread.join();
        assert(tasks.join() && stages == 1);
        assert(!tasks.submit({}, [](TaskReporter) noexcept { return stdexec::just(); }));
        {
            TaskScope empty(runtime);
        }
        {
            TaskScope concurrent(runtime);
            std::latch started{1};
            std::atomic<unsigned> accepted{};
            unsigned settled{};
            std::jthread producer([&] {
                started.count_down();
                for (unsigned i{}; i < 16; ++i)
                {
                    const auto result = concurrent.submit(
                        {"concurrent", "test"},
                        [](TaskReporter) noexcept {
                            return stdexec::just(lux::cxx::expected<void, EExecutionError>{});
                        },
                        [&](TTaskResult<void, EExecutionError>&&) noexcept { ++settled; }
                    );
                    if (result)
                        ++accepted;
                    else
                        assert(result.error() == EExecutionError::STOPPING);
                }
            });
            started.wait();
            concurrent.requestStop();
            producer.join();
            assert(concurrent.join() && settled == accepted.load());
        }
    }
}

int main(int argc, char** argv)
{
    using namespace lux;
    assert(argc == 3);
    auto engine = engine::EngineContext::create({1, 32, 32, {16}, process::BlockingSchedulerConfig{1, 32}}, {1, 1024});
    assert(engine && !(*engine)->renderContext());
    executionChecks((*engine)->execution());
    taskChecks((*engine)->execution());
    taskMeasurements((*engine)->execution());
#if defined(_WIN32)
    taskMemoryAndWait((*engine)->execution());
#endif
    project::PluginCatalog catalog;
    assert(catalog.read(argv[1], argv[2]));
    const auto* description = catalog.find("lux.builtin.runtime");
    assert(description && description->identity.version == 2 && description->dependencies.empty());
    const auto single = catalog.systemsForWorld(project::EMetadataSystemDomain::SCENE, "lux.spatial.builtin.single");
    const auto custom = catalog.systemsForWorld(project::EMetadataSystemDomain::SCENE, "example.custom.partitioner");
    assert(!single.empty() && single.size() == custom.size());
    constexpr std::string_view empty[]{""}, repeated[]{"single", "single"}, mixed[]{"*", "single"};
    system::SystemTypeDescription invalid{"test", 1};
    assert(!system::validSystemTypeDescription(invalid));
    for (auto names :
         {std::span<const std::string_view>{empty},
          std::span<const std::string_view>{repeated},
          std::span<const std::string_view>{mixed}})
    {
        invalid.supported_world_types = names;
        assert(!system::validSystemTypeDescription(invalid));
    }
    auto plugin = project::PluginLibrary::load(*description);
    assert(plugin);
    auto schemas =
        simulation::ecs::ComponentSchemaSet::build({(*plugin)->components().begin(), (*plugin)->components().end()});
    assert(schemas);
    simulation::SimulationSystemRegistry registrations;
    assert(registrations.add((*plugin)->simulationSystems()));
    const auto loading = scene::worldLoadingSystemRegistration();
    scene::WorldLoadingConfiguration config{{{0}}};
    std::vector<std::byte> payload;
    assert(loading.configuration.encode(&config, payload));
    scene::SceneDescriptionBuilder builder;
    assert(builder.addSystem(
        {1},
        "loading",
        loading.type,
        loading.description->version,
        loading.description->configuration_schema_name,
        loading.description->configuration_schema_version,
        payload
    ));
    assert(builder.bindRequirement({1}, "world_loading", "storage"));
    auto scene = std::move(builder).buildResolved();
    assert(scene);
    simulation::SimulationDescriptionBuilder simulation;
    auto rules = std::move(simulation).build();
    assert(rules);
    const auto identity = asset::AssetId{*uuids::uuid::from_string("27279371-1b9c-40d6-b55d-a1a12065d934")};
    auto package = scene::createScenePackage(
        identity,
        "Headless",
        {},
        std::make_shared<const simulation::SimulationDescription>(std::move(*rules)),
        *scene
    );
    assert(package);
    auto source = process::world_loading::makeWorldMemoryStorageSource(package->world->sharedData(), package->volumes);
    assert(source);
    process::TaskScope tasks{(*engine)->execution()};
    scene::WorldLoadingServices services{*source, tasks};
    const auto provider =
        scene::makeSceneCapabilityProvider<scene::WorldLoadingServices>("storage", "lux.world.loading", services);
    auto restricted = loading;
    auto restricted_type = *loading.description;
    constexpr std::string_view supported[]{"example.other.partitioner"};
    restricted_type.supported_world_types = supported;
    restricted.description = &restricted_type;
    auto& scenes = (*engine)->sceneRuntime();
    auto scene_builder = scenes.builder();
    scene_builder.setDescription(package->scene->sharedData())
        .setWorld(package->world->sharedData())
        .setSimulation(package->simulation->sharedData())
        .setRegistrations(*schemas, registrations, std::span(&restricted, 1))
        .setProviders(std::span(&provider, 1));
    const auto rejected = scene_builder.build();
    assert(!rejected);
    const auto& build_error = std::get<scene::SceneBuildFailure>(rejected.error().cause);
    assert(build_error.code == scene::ESceneBuildError::UNSUPPORTED_WORLD_TYPE);
    assert(build_error.scene_system.system == system::SystemInstanceId{1});
    scene_builder.setRegistrations(*schemas, registrations, (*plugin)->sceneSystems());
    const auto instance = scene_builder.build();
    assert(instance && scenes.invalid(*instance));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        assert((*engine)->execution().collectCompletions());
        const auto tick = scenes.tick();
        assert(tick && tick->empty());
        const auto registry = std::as_const(scenes).getSceneRegistry(*instance);
        assert(registry);
        if (registry->get().ctx().get<scene::WorldResidency>().statistics().resident_partitions == 1)
            break;
    }
    assert(scenes.valid(*instance));
    assert(scenes.tick());
    const auto clock = scenes.getClock(*instance);
    assert(clock && std::visit([](const auto& value) { return value.snapshot().step_index; }, clock->get()) == 1);
    assert(scenes.destroy(*instance));
    assert(tasks.join());
    plugin->reset();
    // EngineContext destroys SceneRuntime (including its timer) before ExecutionRuntime.
}
