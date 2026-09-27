#include "ToolTestAccess.hpp"
#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include "EditorTestFactory.hpp"
#include <lux/engine/meta/Meta.hpp>
#include "TestExit.hpp"
#include <cassert>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/editor/ui/Presentation.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>
#include <thread>

// Persistent object identities belong to the Registry's residency state.
static std::vector<lux::simulation::ecs::Entity> authorEntities(lux::editor::scene::SceneEditor& document)
{
    const auto instance = toolTest(document).instance();
    auto& runtime = static_cast<lux::editor::Editor&>(document.root()).context().engine().sceneRuntime();
    const auto& residency =
        std::as_const(runtime).getSceneRegistry(instance)->get().ctx().get<lux::scene::WorldResidency>();
    std::vector<lux::simulation::ecs::Entity> result;
    for (const auto& [id, entity] : residency.identities().entries())
        result.push_back(entity);
    std::ranges::sort(result);
    return result;
}

namespace
{
    template <class T> T identity(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T{uuids::uuid{bytes}};
    }

    void createProject(const std::filesystem::path& root, bool indexed = false)
    {
        using namespace lux;
        using namespace world;
        assert(!std::filesystem::exists(root));
        std::filesystem::create_directories(root);

        simulation::ecs::Registry registry;
        const auto entity = registry.create();
        registry.emplace<consumer::Component>(entity);
        auto capture = consumer::schemas().front().capture(registry, entity, {});
        assert(capture);
        auto payload = capture->encode({}, 1024 * 1024);
        assert(payload);
        const std::array data{WorldEncodedDataRecord{0, 1, *payload}};
        const std::array objects{WorldEncodedObjectRecord{identity<WorldObjectId>(1), data}};
        auto partition = encodeWorldPartitionData(partition::PartitionOrdinal{0}, objects);
        assert(partition);
        const std::array records{WorldPartitionRecord{identity<WorldPartitionId>(5), 0, 1}};
        const std::array extents{WorldPartitionExtent{0, 1, 1}};
        auto table = encodeWorldPartitionTablePage(partition::PartitionOrdinal{0}, records, extents);
        assert(table);
        const auto bundle = identity<WorldBundleId>(6);
        const auto generation = identity<WorldBundleGeneration>(7);
        std::vector chunks{
            WorldStorageChunkInput{EWorldStorageChunkKind::PARTITION_TABLE_PAGE, EWorldStorageCodec::NONE, *table},
            WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, *partition}
        };
        const std::array index_bytes{std::byte{0x41}, std::byte{0x72}};
        if (indexed)
        {
            chunks.push_back({EWorldStorageChunkKind::PARTITION_INDEX_PAGE, EWorldStorageCodec::NONE, index_bytes});
        }
        auto volume = encodeWorldStorageVolume(bundle, generation, 0, chunks);
        assert(volume);

        WorldDescriptionBuilder world;
        assert(world.setIdentity(bundle, generation, "Consumer World"));
        assert(world.addSchema(worldDataSchemaId("consumer.Component")));
        assert(world.setPartitioner({worldPartitionerId("consumer.single"), 1}, 1));
        assert(world.addStorageVolume({"World.wvol", 1, static_cast<std::uint32_t>(chunks.size()), volume->size()}));
        assert(world.addPartitionTablePage({partition::PartitionOrdinal{0}, 1, {0, 0}}));
        if (indexed)
        {
            assert(world.addPartitionIndex({partition::partitionIndexTypeId("consumer.opaque.index"), 1, {0, 2}}));
        }
        auto world_description = std::move(world).build();
        assert(world_description);
        auto world_asset = WorldAsset::create(
            asset::AssetInfo{identity<asset::AssetId>(10)},
            std::make_shared<const WorldDescription>(std::move(*world_description))
        );
        assert(world_asset);

        auto simulation = simulation::SimulationDescriptionBuilder{}.build();
        assert(simulation);
        auto simulation_asset = simulation::SimulationAsset::create(
            asset::AssetInfo{identity<asset::AssetId>(11)},
            std::make_shared<const simulation::SimulationDescription>(std::move(*simulation))
        );
        assert(simulation_asset);
        scene::SceneDescriptionBuilder scene;
        scene.setWorld(identity<asset::AssetId>(10));
        scene.setSimulation(identity<asset::AssetId>(11));
        const auto loading = scene::worldLoadingSystemRegistration();
        scene::WorldLoadingConfiguration loading_configuration{{{0}}};
        std::vector<std::byte> loading_payload;
        assert(loading.configuration.encode(&loading_configuration, loading_payload));
        assert(scene.addSystem(
            {1},
            "loading",
            loading.type,
            loading.description->version,
            loading.description->configuration_schema_name,
            loading.description->configuration_schema_version,
            loading_payload
        ));
        auto scene_description = std::move(scene).build();
        assert(scene_description);
        auto scene_asset = scene::SceneAsset::create(
            asset::AssetInfo{identity<asset::AssetId>(12)},
            std::make_shared<const scene::SceneDescription>(std::move(*scene_description))
        );
        assert(scene_asset);

        std::vector<asset::PakWriteEntry> entries;
        const auto append = [&]<class Asset>(const std::shared_ptr<Asset>& value, const char* path) {
            auto encoded = asset::TAssetSerDeser<std::remove_const_t<Asset>>::encode(
                *value,
                asset::AssetEncodeLimits{16 * 1024 * 1024}
            );
            assert(encoded);
            auto bytes = std::make_shared<std::vector<std::byte>>(std::move(*encoded));
            entries.push_back(
                {value->id(), Asset::primary_magic, path, {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)}
            );
        };
        append(*scene_asset, "Scene");
        append(*world_asset, "World");
        append(*simulation_asset, "Simulation");
        auto bytes = std::make_shared<std::vector<std::byte>>(std::move(*volume));
        entries.push_back(
            {identity<asset::AssetId>(13), 1, "Storage/0", {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)}
        );
        std::string error;
        assert(asset::writePakFile(root / "Main.luxscene", std::move(entries), "/Scene", &error));

        editor::ProjectManifest project{
            identity<asset::AssetId>(20),
            "Installed component",
            "Main.luxscene",
            {{identity<asset::AssetId>(12), editor::EProjectAssetKind::SCENE, "Main.luxscene", {}, {}, {}, "Scenes/Main"
            }}
        };
        project.plugins = {{"lux.builtin.scene_render", 1, {}}};
        auto encoded = editor::encodeProjectManifest(project);
        assert(encoded);
        std::ofstream manifest(root / "Project.luxproject", std::ios::binary);
        manifest << *encoded;
        assert(manifest.good());
    }

    using namespace lux::editor;
    struct Evidence final
    {
        bool complete{};
        bool closed{};
        std::size_t draws{};
        bool indexed{};
        bool measure{};
    };

    template <class Scalar> void checkRotation(scene::SceneEditor& document, lux::simulation::ecs::Entity object)
    {
        using Quaternion = Eigen::Quaternion<Scalar>;
        const auto access = [](auto& component) {
            if constexpr (std::is_same_v<Scalar, float>)
            {
                return &component.rotation_float;
            }
            else
            {
                return &component.rotation_double;
            }
        };
        const auto* component = static_cast<const consumer::Component*>(
            toolTest(document).component(object, lux::cxx::typeToken<consumer::Component>())
        );
        const Quaternion original = *access(*component);
        for (unsigned index = 1; index <= 12; ++index)
        {
            const auto before = document.historyView()->history;
            const auto target = *toolTest(document).writeTarget(object);
            const auto token = toolTest(document).beginFieldEdit<consumer::Component, Quaternion>(
                target,
                "generated-quaternion",
                "rotation",
                "Rotation",
                access
            );
            assert(token);
            const auto update = [&](const Quaternion& value) {
                *access(*const_cast<consumer::Component*>(component)) = value;
                return toolTest(document).fieldEdited(*token);
            };
            // The same Eigen AngleAxis composition and normalization emitted by the generator.
            const Quaternion rotation = Quaternion{
                Eigen::AngleAxis<Scalar>{Scalar(0.13 * index), Eigen::Matrix<Scalar, 3, 1>::UnitX()} *
                Eigen::AngleAxis<Scalar>{Scalar(0.29 * index), Eigen::Matrix<Scalar, 3, 1>::UnitY()} *
                Eigen::AngleAxis<Scalar>{Scalar(0.37 * index), Eigen::Matrix<Scalar, 3, 1>::UnitZ()}
            }.normalized();
            assert(scene::TFieldValue<Quaternion>::valid(rotation));
            assert(update(rotation));
            Quaternion invalid = rotation;
            invalid.coeffs() *= Scalar(1.01);
            const auto rejected = update(invalid);
            assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED);
            assert(document.historyView()->history.current == before.current);
            assert(scene::TFieldValue<Quaternion>::equal(*access(*component), original));
            invalid.coeffs().setZero();
            assert(!update(invalid));
            invalid.w() = std::numeric_limits<Scalar>::quiet_NaN();
            assert(!update(invalid));
            assert(update(rotation));
            assert(toolTest(document).finishFieldEdit(*token));
            assert(document.undo() && scene::TFieldValue<Quaternion>::equal(*access(*component), original));
            assert(document.redo() && scene::TFieldValue<Quaternion>::equal(*access(*component), rotation));
            assert(document.undo() && document.historyView()->history.current == before.current);
        }
        std::printf(
            "PASS typed generated Quaternion%s: twelve Eigen rotations, invalid direct input restores initial "
            "value, Undo/Redo\n",
            std::is_same_v<Scalar, float> ? "f" : "d"
        );
    }

    void checkVectorElements(scene::SceneEditor& document, lux::simulation::ecs::Entity object)
    {
        const auto whole = [](auto& component) noexcept { return &component.sequence; };
        const auto read = [&]() -> const consumer::Component& {
            return *static_cast<const consumer::Component*>(
                toolTest(document).component(object, lux::cxx::typeToken<consumer::Component>())
            );
        };
        const auto original = read().sequence;
        const auto initial = document.historyView()->history.current;
        for (const std::size_t count : {256U, 4096U})
        {
            std::vector<consumer::Settings> values(count);
            assert(toolTest(document).setField<consumer::Component>(
                *toolTest(document).writeTarget(object),
                "sequence",
                "Sequence",
                whole,
                values
            ));
            const auto element = [count](auto& component) noexcept {
                using Item = std::remove_reference_t<decltype(component.sequence.front())>;
                return component.sequence.size() == count ? &component.sequence.front() : static_cast<Item*>(nullptr);
            };
            const auto before = document.historyView()->history;
            auto preview = toolTest(document).beginFieldEdit<consumer::Component, consumer::Settings>(
                *toolTest(document).writeTarget(object),
                "vector-element-test",
                "sequence[0]",
                "[0]",
                element
            );
            assert(preview);
            auto next = values.front();
            const auto begin = std::chrono::steady_clock::now();
            for (unsigned update = 1; update <= 100; ++update)
            {
                next.gain = 0.01 * update;
                auto& live = const_cast<consumer::Component&>(read()).sequence.front();
                live.gain = next.gain;
                assert(toolTest(document).fieldEdited(*preview));
            }
#if defined(CONSUMER_MEASURE_COPIES)
            const auto copies_before = consumer::settings_copies.load(std::memory_order_relaxed);
#endif
            assert(toolTest(document).finishFieldEdit(*preview));
#if defined(CONSUMER_MEASURE_COPIES)
            const auto copies = consumer::settings_copies.load(std::memory_order_relaxed) - copies_before;
            assert(copies == CONSUMER_EXPECT_ADOPT_COPIES);
            std::printf(
                "DIAGNOSTIC adopted preview copies=%llu size=%zu\n",
                static_cast<unsigned long long>(copies),
                count
            );
#endif
            const auto elapsed =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
            const auto applied = document.historyView()->history;
            assert(applied.cursor == before.cursor + 1 && read().sequence.front().gain == 1.0);
            assert(read().sequence.back().gain == 1.5);
            const auto retained = applied.charged_retained_bytes - before.charged_retained_bytes;
            assert(retained < 4096);

            // A structural edit and its Undo change the temporary component version.
            // Earlier element history must still replay after the exact structure returns.
            auto resized = read().sequence;
            resized.emplace_back();
            assert(toolTest(document).setField<consumer::Component>(
                *toolTest(document).writeTarget(object),
                "sequence",
                "Sequence",
                whole,
                resized
            ));
            assert(document.undo() && read().sequence.size() == count);
            assert(document.undo() && read().sequence.front().gain == 1.5);
            assert(document.redo() && read().sequence.front().gain == 1.0);
            assert(document.redo() && read().sequence.size() == count + 1);
            assert(document.undo() && document.undo() && document.undo());
            assert(document.historyView()->history.current == initial);
            assert(scene::TFieldValue<std::vector<consumer::Settings>>::equal(read().sequence, original));
            std::printf(
                "PASS vector element: size=%zu updates=100 entries=1 retained=%zu active_us=%.3f "
                "structural-undo-then-element-replay=1\n",
                count,
                retained,
                elapsed
            );
        }
    }

    class Probe final
    {
        TestExit exit_;

    public:
        explicit Probe(Evidence& evidence) : evidence_(evidence) {}
        void bind(Editor& editor) noexcept
        {
            editor_ = &editor;
        }

        void poll()
        {
            exit_.poll();
            assert(std::chrono::steady_clock::now() - began_ < std::chrono::seconds(60));
            if (stage_ == 3 && !tool_->historyId().value)
            {
                assert(tool_->openAsset(identity<lux::asset::AssetId>(12)));
                stage_ = 4;
                return;
            }
            if (tool_->assetStatus().phase != EAssetEditPhase::IDLE || !tool_->historyId().value)
                return;
            assert(!tool_->assetStatus().failure);
            auto& document = *tool_;
            auto object = authorEntities(document).front();
            const auto read = [&]() -> const consumer::Component& {
                const auto* value = static_cast<const consumer::Component*>(
                    toolTest(document).component(object, lux::cxx::typeToken<consumer::Component>())
                );
                assert(value);
                return *value;
            };
            const auto map_access = [](auto& value) { return &value.map; };
            if (stage_ == 0)
            {
                assert(toolTest(document).select(object));
                assert(read().map.at("key").size() == 2);
                stage_ = 1;
            }
            if (stage_ == 1 && consumer::drawCount() >= 3)
            {
                if (evidence_.indexed)
                {
                    const auto before = document.historyView()->history;
                    assert(!document.writeRestriction().empty());
                    const auto target = toolTest(document).writeTarget(object);
                    assert(!target && target.error().code == editing::EEditError::BLOCKED_BY_HOST);
                    assert(!toolTest(document).createObject(
                        before.current,
                        lux::partition::PartitionOrdinal{0},
                        scene::EObjectSpace::NONE
                    ));
                    const auto save = document.requestSave("indexed");
                    assert(!save && save.error().code == EEditorError::READ_ONLY);
                    assert(
                        document.historyView()->history.current == before.current &&
                        document.historyView()->history.revision == before.revision
                    );
                    evidence_.complete = true;
                    evidence_.draws = consumer::drawCount();
                    stage_ = 5;
                    exit_.request(*editor_);
                    std::puts("PASS indexed World: real source remains inspectable, field/structure/save refused "
                              "before effects");
                    return;
                }
                if (evidence_.measure && sample_stage_ < 3)
                {
                    const auto sequence = [](auto& value) noexcept { return &value.sequence; };
                    if (sample_stage_ == 0)
                    {
                        assert(toolTest(document).setField<consumer::Component>(
                            *toolTest(document).writeTarget(object),
                            "sequence",
                            "Sequence",
                            sequence,
                            std::vector<consumer::Settings>(256)
                        ));
                        consumer::beginDrawSample();
                        sample_stage_ = 1;
                        return;
                    }
                    const auto sample = consumer::drawSample();
                    if (sample.draws != 100)
                    {
                        return;
                    }
                    const auto count = sample_stage_ == 1 ? 256U : 4096U;
                    const auto& value = read().sequence;
                    assert(value.size() == count);
                    double checksum{};
                    for (const auto& element : value)
                    {
                        checksum += element.gain;
                    }
                    assert(checksum == 1.5 * count);
                    std::printf(
                        "MEASURE generated-inspector size=%u warmup=%zu draws=%zu active_us=%.3f "
                        "checksum=%.1f width=1000 height=700 scene_views=1\n",
                        count,
                        sample.warmup,
                        sample.draws,
                        sample.active_microseconds,
                        checksum
                    );
                    assert(document.undo());
                    ++sample_stage_;
                    if (sample_stage_ == 2)
                    {
                        assert(toolTest(document).setField<consumer::Component>(
                            *toolTest(document).writeTarget(object),
                            "sequence",
                            "Sequence",
                            sequence,
                            std::vector<consumer::Settings>(4096)
                        ));
                        consumer::beginDrawSample();
                        return;
                    }
                }
                consumer::checkUndrawnInspector(
                    document,
                    toolTest(document).editing(),
                    toolTest(document).history(),
                    toolTest(document).selection().object,
                    [&] {
                        assert(document.content());
                        assert(document.root().update({}, nullptr));
                    }
                );
                checkRotation<float>(document, object);
                checkRotation<double>(document, object);
                checkVectorElements(document, object);
                const auto before = read();
                const auto initial = document.historyView()->history.current;
                assert(toolTest(document).eraseObjects(initial, std::span(&object, 1)));
                assert(authorEntities(document).empty());
                const auto deleted = document.historyView()->history;
                consumer::rejectNextDecode();
                const auto rejected = document.undo();
                assert(
                    !rejected &&
                    rejected.error().domain_code == static_cast<unsigned>(scene::ESceneStructureError::CODEC_FAILURE)
                );
                assert(authorEntities(document).empty() && document.historyView()->history.current == deleted.current);
                assert(
                    std::string_view(rejected.error().message.data()) == "consumer.Component: decode error 1 at byte 23"
                );
                assert(document.historyView()->history.revision == deleted.revision);
                assert(document.historyView()->history.cursor == deleted.cursor);
                for (unsigned iteration{}; iteration < 8; ++iteration)
                {
                    assert(document.undo());
                    assert(!toolTest(document).writeTarget(object));
                    object = authorEntities(document).front();
                    assert(scene::TFieldValue<consumer::Component>::equal(read(), before));
                    assert(document.redo() && authorEntities(document).empty());
                }
                assert(document.undo() && document.historyView()->history.current == initial);
                assert(!toolTest(document).writeTarget(object));
                object = authorEntities(document).front();
                std::puts(
                    "PASS installed structural restore: generated nested component, eight delete/Undo/Redo cycles, "
                    "same authored value and identity"
                );
                auto changed = read().map;
                changed.emplace("new entry", std::vector{8, 9});
                auto target = toolTest(document).writeTarget(object);
                assert(target);
                assert(toolTest(document).setField<consumer::Component>(*target, "map", "Map", map_access, changed));
                assert(document.undo() && !read().map.contains("new entry"));
                assert(document.redo() && read().map.contains("new entry"));
                auto save = document.requestSave("installed-consumer");
                assert(save);
                save_ = *save;
                changed.at("new entry").push_back(10);
                target = toolTest(document).writeTarget(object);
                assert(target);
                assert(toolTest(document).setField<consumer::Component>(*target, "map", "Map", map_access, changed));
                stage_ = 2;
            }
            if (stage_ == 2)
            {
                auto status = document.saveStatus(save_);
                assert(status && !std::holds_alternative<SaveRetryable>(*status));
                if (std::holds_alternative<SaveSucceeded>(*status))
                {
                    assert(!document.historyView()->history.clean);
                    assert(document.undo() && document.historyView()->history.clean);
                    assert(document.acknowledgeSave(save_));
                    const auto target = *toolTest(document).writeTarget(object);
                    const auto original = read().map;
                    assert(document.clearAsset());
                    if (document.assetStatus().phase == EAssetEditPhase::REVIEW)
                        assert(document.reviewAsset(EAssetChangeDecision::DISCARD));
                    stage_ = 3;
                }
            }
            if (stage_ == 4)
            {
                if (document.assetStatus().phase == EAssetEditPhase::IDLE)
                {
                    assert(read().map.at("new entry") == std::vector({8, 9}));
                    assert(document.historyView()->history.clean);
                    evidence_.complete = true;
                    evidence_.draws = consumer::drawCount();
                    stage_ = 5;
                    exit_.request(*editor_);
                }
            }
        }

    public:
        EditorConfig configuration()
        {
            EditorConfig config;
            config.plugin_root = LUX_TEST_PLUGIN_ROOT;
            config.window.visible = false;
            config.window.width = 1000;
            config.window.height = 700;
            config.window.title = "D2 installed generated component";
            return config;
        }

        void configure()
        {
            auto& context = editor_->context();
            resources_ = &context.renderResources();
            renderer_ = &context.renderRuntime();
            runtime_ = &context.execution();
            project_ = &context.project();
            auto metadata = sceneRegistrations(consumer::schemas(), context.plugins().libraries());
            assert(metadata);
            auto bindings = ui::componentEditors();
            bindings.push_back(consumer::binding());
            auto editors = ComponentEditorRegistry::create(metadata->components, std::move(bindings));
            assert(editors);
            // Test setup only, before a tool or generated component editor borrows the fixed catalog.
            const_cast<SceneRegistrations&>(context.sceneRegistrations()) = std::move(*metadata);
            const_cast<ComponentEditorRegistry&>(context.componentEditors()) = std::move(*editors);
            auto tool = EditorTestFactory::show<scene::SceneEditor>(*editor_);
            assert(tool);
            tool_ = &tool->get();
            assert(tool_->openAsset(identity<lux::asset::AssetId>(12)));
            enqueue();
        }

    private:
        void enqueue()
        {
            const auto posted = lux::object::detail::post(
                project_->dispatcherRef(),
                lux::object::detail::makeMessage([this]() noexcept {
                    if (editor_->closing())
                    {
                        return;
                    }
                    poll();
                    if (!editor_->closing())
                    {
                        enqueue();
                    }
                })
            );
            assert(posted == lux::object::detail::EPostStatus::POSTED);
        }

        Evidence& evidence_;
        Editor* editor_{};
        ProjectStorage* project_{};
        lux::render::RenderRuntime* renderer_{};
        lux::scene::RenderResources* resources_{};
        lux::process::ExecutionRuntime* runtime_{};
        SaveRequestId save_;
        scene::SceneEditor* tool_{};
        std::uint32_t stage_{};
        std::uint32_t sample_stage_{};
        std::chrono::steady_clock::time_point began_{std::chrono::steady_clock::now()};
    };

    void checkClosingPublication(const std::filesystem::path& project_file, bool writable = true)
    {
        auto runtime =
            lux::process::ExecutionRuntime::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}});
        assert(runtime && runtime->blocking());
        lux::process::TaskScope tasks{*runtime};
        auto messages_created = lux::object::ObjectMessageQueue::create(64);
        assert(messages_created);
        auto messages = std::move(*messages_created);
        auto source = readProjectOpenData(project_file);
        assert(source);
        lux::asset::AssetVfs assets;
        auto project = ProjectStorage::open(*source, assets, *runtime->blocking(), tasks, messages.dispatcherRef());
        assert(project && (*project)->writable() == writable);
        if (!writable)
            return; // The live Editor still owns the exclusive project lease after exec.
        ProjectUpdate update;
        auto reservation = (*project)->preparePublication(update);
        assert(reservation);
        (*project)->requestClose();
        const auto closing = (*project)->advanceClose();
        assert(closing && !*closing);
        bool released{};
        assert(tasks.submit(
            {"Release publication", "test"},
            [](lux::process::TaskReporter) noexcept {
                return stdexec::just(lux::cxx::expected<void, lux::process::EExecutionError>{});
            },
            [&](lux::process::TTaskResult<void, lux::process::EExecutionError>&& result) noexcept {
                assert(result);
                assert((*project)->manifest().name == "Installed component");
                *reservation = {};
                released = true;
            }
        ));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (true)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            assert(runtime->collectCompletions());
            const auto closed = (*project)->advanceClose();
            assert(closed);
            if (*closed)
            {
                break;
            }
        }
        assert(released);
        project->reset();
        assert(tasks.join());
        messages.close();
        runtime->requestStop();
        assert(runtime->join());
        std::puts("PASS Project close: incomplete result retained owner until queued Main publication release");
    }
} // namespace

int sceneWorkflow(const std::filesystem::path& root, bool measure)
{
    createProject(root);
    // Each application owns its reflection/plugin lifetime. Finish the first
    // application before starting the independent indexed-project application.
    {
        Evidence evidence;
        evidence.measure = measure;
        Probe probe(evidence);
        auto config = probe.configuration();
        config.project_file = root / "Project.luxproject";
        config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
        auto ui_messages_created = lux::object::ObjectMessageQueue::create(64);
        assert(ui_messages_created);
        auto ui_messages = std::move(*ui_messages_created);
        auto created_editor = lux::editor::EditorTestFactory::create(std::move(config));
        assert(created_editor);
        auto& editor = **created_editor;
        probe.bind(editor);
        probe.configure();
        const auto result = editor.exec();
        evidence.closed = editor.closing();
        if (result || !evidence.complete || !evidence.closed)
        {
            if (!editor.outcome())
            {
                std::fprintf(
                    stderr,
                    "FAIL %s:%llu %s\n",
                    editor.outcome().error().domain.c_str(),
                    editor.outcome().error().reason,
                    editor.outcome().error().message.c_str()
                );
            }
            return 1;
        }
        std::printf(
            "PASS installed generated Scene Inspector: draws=%zu, map edit, Undo/Redo, fixed capture save, reopen, "
            "normal close\n",
            evidence.draws
        );

        assert(editor.context().project().writable());
        checkClosingPublication(root / "Project.luxproject", false);
    }
    checkClosingPublication(root / "Project.luxproject"); // Scope exit releases the project lease.

    const auto indexed_root = root / "indexed";
    createProject(indexed_root, true);
    const auto original_source = projectFileDigest(indexed_root / "Main.luxscene");
    assert(original_source);
    Evidence indexed;
    indexed.indexed = true;
    Probe indexed_probe(indexed);
    auto indexed_config = indexed_probe.configuration();
    indexed_config.project_file = indexed_root / "Project.luxproject";
    indexed_config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    auto indexed_messages_created = lux::object::ObjectMessageQueue::create(64);
    assert(indexed_messages_created);
    auto indexed_messages = std::move(*indexed_messages_created);
    auto indexed_created = EditorTestFactory::create(std::move(indexed_config));
    assert(indexed_created);
    auto& indexed_editor = **indexed_created;
    indexed_probe.bind(indexed_editor);
    indexed_probe.configure();
    assert(indexed_editor.exec() == 0 && indexed.complete && indexed_editor.closing());
    const auto retained_source = projectFileDigest(indexed_root / "Main.luxscene");
    assert(retained_source && *retained_source == *original_source);
    return 0;
}
