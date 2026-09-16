#include "TestExit.hpp"
#include <cassert>
#include <consumer/Domain.hpp>
#include <consumer/Gui.hpp>
#include <cstdio>
#include <fstream>
#include <lux/engine/editor/Editor.hpp>
#include <lux/engine/editor/gui/GuiFrontend.hpp>
#include <lux/engine/editor/gui/scene/SceneDocumentProvider.hpp>
#include <lux/engine/editor/scene/FieldEdit.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationAssetCodec.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>
#include <lux/engine/world/WorldAssetCodec.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>
#include <lux/engine/world/WorldStorageCodec.hpp>

namespace
{
    template <class T> T identity(std::uint8_t value)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = value;
        return T{uuids::uuid{bytes}};
    }

    void createProject(const std::filesystem::path &root, bool indexed = false)
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
            WorldStorageChunkInput{EWorldStorageChunkKind::WORLD_PARTITION_DATA, EWorldStorageCodec::NONE, *partition}};
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
        auto world_asset = WorldAsset::create(asset::AssetInfo{identity<asset::AssetId>(10)},
                                              std::make_shared<const WorldDescription>(std::move(*world_description)));
        assert(world_asset);

        auto simulation = simulation::SimulationDescriptionBuilder{}.build();
        assert(simulation);
        auto simulation_asset = simulation::SimulationAsset::create(
            asset::AssetInfo{identity<asset::AssetId>(11)},
            std::make_shared<const simulation::SimulationDescription>(std::move(*simulation)));
        assert(simulation_asset);
        scene::SceneDescriptionBuilder scene;
        scene.setWorld(identity<asset::AssetId>(10));
        scene.setSimulation(identity<asset::AssetId>(11));
        auto scene_description = std::move(scene).build();
        assert(scene_description);
        auto scene_asset =
            scene::SceneAsset::create(asset::AssetInfo{identity<asset::AssetId>(12)},
                                      std::make_shared<const scene::SceneDescription>(std::move(*scene_description)));
        assert(scene_asset);

        std::vector<asset::PakWriteEntry> entries;
        const auto append = [&]<class Asset>(const std::shared_ptr<Asset> &value, const char *path)
        {
            auto encoded = asset::TAssetSerDeser<std::remove_const_t<Asset>>::encode(
                *value, asset::AssetEncodeLimits{16 * 1024 * 1024});
            assert(encoded);
            auto bytes = std::make_shared<std::vector<std::byte>>(std::move(*encoded));
            entries.push_back(
                {value->id(), Asset::primary_magic, path, {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)});
        };
        append(*scene_asset, "Scene");
        append(*world_asset, "World");
        append(*simulation_asset, "Simulation");
        auto bytes = std::make_shared<std::vector<std::byte>>(std::move(*volume));
        entries.push_back(
            {identity<asset::AssetId>(13), 1, "Storage/0", {}, cxx::SharedBytes<>::fromOwner(bytes, *bytes)});
        std::string error;
        assert(asset::writePakFile(root / "Main.luxscene", std::move(entries), "/Scene", &error));

        editor::ProjectManifest project{identity<asset::AssetId>(20),
                                        "Installed component",
                                        "Main.luxscene",
                                        {{identity<asset::AssetId>(12),
                                          editor::EProjectAssetKind::SCENE,
                                          "Main.luxscene",
                                          {},
                                          {},
                                          {},
                                          "Scenes/Main"}}};
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

    template <class Scalar> void checkRotation(scene::SceneEditor &document, lux::world::WorldObjectId object)
    {
        using Quaternion = Eigen::Quaternion<Scalar>;
        const auto access = [](auto &component)
        {
            if constexpr (std::is_same_v<Scalar, float>)
            {
                return &component.rotation_float;
            }
            else
            {
                return &component.rotation_double;
            }
        };
        const auto *component = static_cast<const consumer::Component *>(
            document.component(object, lux::cxx::typeToken<consumer::Component>()));
        const Quaternion original = *access(*component);
        for (unsigned index = 1; index <= 12; ++index)
        {
            const auto before = document.historyView()->history;
            const auto target = *document.writeTarget(object);
            const auto token = document.beginPreview<consumer::Component, Quaternion>(target, "generated-quaternion",
                                                                                      "rotation", "Rotation", access);
            assert(token);
            // The same Eigen AngleAxis composition and normalization emitted by the generator.
            const Quaternion rotation =
                Quaternion{Eigen::AngleAxis<Scalar>{Scalar(0.13 * index), Eigen::Matrix<Scalar, 3, 1>::UnitX()} *
                           Eigen::AngleAxis<Scalar>{Scalar(0.29 * index), Eigen::Matrix<Scalar, 3, 1>::UnitY()} *
                           Eigen::AngleAxis<Scalar>{Scalar(0.37 * index), Eigen::Matrix<Scalar, 3, 1>::UnitZ()}}
                    .normalized();
            assert(scene::FieldValue<Quaternion>::valid(rotation));
            assert(document.updatePreview(*token, rotation));
            Quaternion invalid = rotation;
            invalid.coeffs() *= Scalar(1.01);
            const auto rejected = document.updatePreview(*token, invalid);
            assert(!rejected && rejected.error().code == editing::EEditError::PRECONDITION_FAILED);
            assert(document.historyView()->history.current == before.current);
            assert(scene::FieldValue<Quaternion>::equal(*access(*component), rotation));
            invalid.coeffs().setZero();
            assert(!document.updatePreview(*token, invalid));
            invalid.w() = std::numeric_limits<Scalar>::quiet_NaN();
            assert(!document.updatePreview(*token, invalid));
            assert(document.commitPreview(*token));
            assert(document.undo() && scene::FieldValue<Quaternion>::equal(*access(*component), original));
            assert(document.redo() && scene::FieldValue<Quaternion>::equal(*access(*component), rotation));
            assert(document.undo() && document.historyView()->history.current == before.current);
        }
        std::printf(
            "PASS typed generated Quaternion%s: twelve Eigen rotations, preview rejection retains value, Undo/Redo\n",
            std::is_same_v<Scalar, float> ? "f" : "d");
    }

    void checkVectorElements(scene::SceneEditor &document, lux::world::WorldObjectId object)
    {
        const auto whole = [](auto &component) noexcept { return &component.sequence; };
        const auto read = [&]() -> const consumer::Component &
        {
            return *static_cast<const consumer::Component *>(
                document.component(object, lux::cxx::typeToken<consumer::Component>()));
        };
        const auto original = read().sequence;
        const auto initial = document.historyView()->history.current;
        for (const std::size_t count : {256U, 4096U})
        {
            std::vector<consumer::Settings> values(count);
            assert(document.setField<consumer::Component>(*document.writeTarget(object), "sequence", "Sequence", whole,
                                                          values));
            const auto element = [count](auto &component) noexcept
            {
                using Item = std::remove_reference_t<decltype(component.sequence.front())>;
                return component.sequence.size() == count ? &component.sequence.front() : static_cast<Item *>(nullptr);
            };
            const auto before = document.historyView()->history;
            auto preview = document.beginPreview<consumer::Component, consumer::Settings>(
                *document.writeTarget(object), "vector-element-test", "sequence[0]", "[0]", element);
            assert(preview);
            auto next = values.front();
            const auto begin = std::chrono::steady_clock::now();
            for (unsigned update = 1; update <= 100; ++update)
            {
                next.gain = 0.01 * update;
                assert(document.updatePreview(*preview, next));
            }
#if defined(CONSUMER_MEASURE_COPIES)
            const auto copies_before = consumer::settings_copies.load(std::memory_order_relaxed);
#endif
            assert(document.commitPreview(*preview));
#if defined(CONSUMER_MEASURE_COPIES)
            const auto copies = consumer::settings_copies.load(std::memory_order_relaxed) - copies_before;
            assert(copies == CONSUMER_EXPECT_ADOPT_COPIES);
            std::printf("DIAGNOSTIC adopted preview copies=%llu size=%zu\n", static_cast<unsigned long long>(copies),
                        count);
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
            assert(document.setField<consumer::Component>(*document.writeTarget(object), "sequence", "Sequence", whole,
                                                          resized));
            assert(document.undo() && read().sequence.size() == count);
            assert(document.undo() && read().sequence.front().gain == 1.5);
            assert(document.redo() && read().sequence.front().gain == 1.0);
            assert(document.redo() && read().sequence.size() == count + 1);
            assert(document.undo() && document.undo() && document.undo());
            assert(document.historyView()->history.current == initial);
            assert(scene::FieldValue<std::vector<consumer::Settings>>::equal(read().sequence, original));
            std::printf("PASS vector element: size=%zu updates=100 entries=1 retained=%zu active_us=%.3f "
                        "structural-undo-then-element-replay=1\n",
                        count, retained, elapsed);
        }
    }

    class Probe final : public EditorFrontend
    {
        TestExit exit_;

      public:
        explicit Probe(Evidence &evidence) : evidence_(evidence), frontend_(gui::makeGuiFrontend(configuration())) {}

        EditorResult<void> beginStartup(Editor &editor, lux::process::ExecutionRuntime &runtime,
                                        lux::object::ObjectDispatcherRef dispatcher) override
        {
            editor_ = &editor;
            return frontend_->beginStartup(editor, runtime, dispatcher);
        }

        EditorResult<void> enterProject(Editor &editor, Project &project, lux::process::ExecutionRuntime &runtime,
                                        lux::object::ObjectDispatcherRef dispatcher) override
        {
            project_ = &project;
            return frontend_->enterProject(editor, project, runtime, dispatcher);
        }

        void collectInput(Editor &editor) override
        {
            frontend_->collectInput(editor);
        }
        void draw(Editor &editor, PollBudget &budget) override
        {
            frontend_->draw(editor, budget);
        }
        void wait() override
        {
            frontend_->wait();
        }
        void stopPresenting() noexcept override
        {
            frontend_->stopPresenting();
        }
        void requestClose() noexcept override
        {
            closing_ = true;
            frontend_->requestClose();
        }
        CloseStatus closeStatus() const override
        {
            return frontend_->closeStatus();
        }

        void poll(PollBudget &budget) override
        {
            exit_.poll();
            frontend_->poll(budget);
            if (closing_)
            {
                evidence_.closed = frontend_->closeStatus().state == ECloseState::CLOSED;
                return;
            }
            assert(std::chrono::steady_clock::now() - began_ < std::chrono::seconds(60));
            const auto documents = editor_->documents();
            if (stage_ == 3 && documents.empty())
            {
                auto request = editor_->requestOpen({{project_->manifest().id, identity<lux::asset::AssetId>(12),
                                                      std::string(scene::kSceneDocumentType)},
                                                     "installed-reopen"});
                assert(request);
                request_ = *request;
                stage_ = 4;
                return;
            }
            if (documents.empty())
            {
                return;
            }
            auto borrowed = editor_->document(documents.front().handle);
            assert(borrowed);
            auto &document = dynamic_cast<scene::SceneEditor &>(borrowed->get());
            const auto object = identity<lux::world::WorldObjectId>(1);
            const auto read = [&]() -> const consumer::Component &
            {
                const auto *value = static_cast<const consumer::Component *>(
                    document.component(object, lux::cxx::typeToken<consumer::Component>()));
                assert(value);
                return *value;
            };
            const auto map_access = [](auto &value) { return &value.map; };
            if (stage_ == 0)
            {
                assert(document.select(object));
                assert(read().map.at("key").size() == 2);
                stage_ = 1;
            }
            if (stage_ == 1 && consumer::drawCount() >= 3)
            {
                if (evidence_.indexed)
                {
                    const auto before = document.historyView()->history;
                    assert(document.summary().read_only && !document.summary().read_only_reason.empty());
                    const auto target = document.writeTarget(object);
                    assert(!target && target.error().code == editing::EEditError::BLOCKED_BY_HOST);
                    assert(!document.createObject(before.current, lux::partition::PartitionOrdinal{0},
                                                  scene::EObjectSpace::NONE));
                    const auto save = document.requestSave("indexed");
                    assert(!save && save.error().code == EEditorError::READ_ONLY);
                    assert(document.historyView()->history.current == before.current &&
                           document.historyView()->history.revision == before.revision);
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
                    const auto sequence = [](auto &value) noexcept { return &value.sequence; };
                    if (sample_stage_ == 0)
                    {
                        assert(document.setField<consumer::Component>(*document.writeTarget(object), "sequence",
                                                                      "Sequence", sequence,
                                                                      std::vector<consumer::Settings>(256)));
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
                    const auto &value = read().sequence;
                    assert(value.size() == count);
                    double checksum{};
                    for (const auto &element : value)
                    {
                        checksum += element.gain;
                    }
                    assert(checksum == 1.5 * count);
                    std::printf("MEASURE generated-inspector size=%u warmup=%zu draws=%zu active_us=%.3f "
                                "checksum=%.1f width=1000 height=700 scene_views=1\n",
                                count, sample.warmup, sample.draws, sample.active_microseconds, checksum);
                    assert(document.undo());
                    ++sample_stage_;
                    if (sample_stage_ == 2)
                    {
                        assert(document.setField<consumer::Component>(*document.writeTarget(object), "sequence",
                                                                      "Sequence", sequence,
                                                                      std::vector<consumer::Settings>(4096)));
                        consumer::beginDrawSample();
                        return;
                    }
                }
                consumer::checkUndrawnInspector(document, *window_);
                checkRotation<float>(document, object);
                checkRotation<double>(document, object);
                checkVectorElements(document, object);
                const auto before = read();
                const auto initial = document.historyView()->history.current;
                assert(document.eraseObjects(initial, std::span(&object, 1)));
                assert(document.objects().empty());
                const auto deleted = document.historyView()->history;
                consumer::rejectNextDecode();
                const auto rejected = document.undo();
                assert(!rejected && rejected.error().domain_code ==
                                        static_cast<unsigned>(scene::ESceneStructureError::CODEC_FAILURE));
                assert(document.objects().empty() && document.historyView()->history.current == deleted.current);
                assert(std::string_view(rejected.error().message.data()) ==
                       "consumer.Component: decode error 1 at byte 23");
                assert(document.historyView()->history.revision == deleted.revision);
                assert(document.historyView()->history.cursor == deleted.cursor);
                for (unsigned iteration{}; iteration < 8; ++iteration)
                {
                    assert(document.undo());
                    assert(scene::FieldValue<consumer::Component>::equal(read(), before));
                    assert(document.redo() && document.objects().empty());
                }
                assert(document.undo() && document.historyView()->history.current == initial);
                std::puts(
                    "PASS installed structural restore: generated nested component, eight delete/Undo/Redo cycles, "
                    "same authored value and identity");
                auto changed = read().map;
                changed.emplace("new entry", std::vector{8, 9});
                auto target = document.writeTarget(object);
                assert(target);
                assert(document.setField<consumer::Component>(*target, "map", "Map", map_access, changed));
                assert(document.undo() && !read().map.contains("new entry"));
                assert(document.redo() && read().map.contains("new entry"));
                auto save = document.requestSave("installed-consumer");
                assert(save);
                save_ = *save;
                changed.at("new entry").push_back(10);
                target = document.writeTarget(object);
                assert(target);
                assert(document.setField<consumer::Component>(*target, "map", "Map", map_access, changed));
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
                    const auto target = *document.writeTarget(object);
                    const auto original = read().map;
                    const auto before_close = document.historyView()->history;
                    document.requestClose();
                    const auto rejected =
                        document.setField<consumer::Component>(target, "map", "Map", map_access, original);
                    assert(!rejected && rejected.error().code == editing::EEditError::CLOSED);
                    assert(!document.undo() && !document.redo());
                    const auto save_after_close = document.requestSave("after-close");
                    assert(!save_after_close && save_after_close.error().code == EEditorError::CLOSING);
                    assert(document.historyView()->history.current == before_close.current &&
                           document.historyView()->history.revision == before_close.revision);
                    std::puts("PASS Scene close intent: fields, history and new save rejected with unchanged "
                              "content/history");
                    stage_ = 3;
                }
            }
            if (stage_ == 4)
            {
                const auto status = editor_->openStatus(request_);
                assert(status);
                if (std::holds_alternative<DocumentHandle>(*status))
                {
                    assert(read().map.at("new entry") == std::vector({8, 9}));
                    assert(document.historyView()->history.clean);
                    assert(editor_->acknowledgeOpen(request_));
                    evidence_.complete = true;
                    evidence_.draws = consumer::drawCount();
                    stage_ = 5;
                    exit_.request(*editor_);
                }
            }
        }

      private:
        gui::GuiConfig configuration()
        {
            gui::GuiConfig config;
            config.window.width = 1000;
            config.window.height = 700;
            config.window.title = "D2 installed generated component";
            const std::array bindings{consumer::binding()};
            auto provider = gui::sceneDocumentProvider(consumer::schemas(), bindings);
            const auto attach = provider.attach;
            provider.attach = [this, attach](DocumentEditor &document, gui::EditorWindow &window,
                                             rendering::EditorRenderer &renderer,
                                             lux::process::ExecutionRuntime &runtime)
            {
                window_ = &window;
                return attach(document, window, renderer, runtime);
            };
            config.providers.push_back(std::move(provider));
            return config;
        }

        Evidence &evidence_;
        std::unique_ptr<EditorFrontend> frontend_;
        Editor *editor_{};
        Project *project_{};
        gui::EditorWindow *window_{};
        SaveRequestId save_;
        OpenRequestId request_;
        std::uint32_t stage_{};
        std::uint32_t sample_stage_{};
        bool closing_{};
        std::chrono::steady_clock::time_point began_{std::chrono::steady_clock::now()};
    };

    struct ClosingPublication final
    {
        ProjectPublication reservation;
        lux::process::TaskScope tasks;
        bool retained{};
        bool released{};
    };

    class ClosingFrontend final : public EditorFrontend
    {
        TestExit exit_;

      public:
        explicit ClosingFrontend(ClosingPublication &publication) : publication_(publication) {}
        EditorResult<void> beginStartup(Editor &, lux::process::ExecutionRuntime &runtime,
                                        lux::object::ObjectDispatcherRef) override
        {
            runtime_ = &runtime;
            return {};
        }
        EditorResult<void> enterProject(Editor &editor, Project &project, lux::process::ExecutionRuntime &,
                                        lux::object::ObjectDispatcherRef) override
        {
            ProjectUpdate update;
            auto reserved = project.preparePublication(update);
            assert(reserved);
            publication_.reservation = std::move(*reserved);
            project_ = &project;
            exit_.request(editor);
            return {};
        }
        void collectInput(Editor &) override {}
        void poll(PollBudget &) override
        {
            exit_.poll();
        }
        void draw(Editor &, PollBudget &) override {}
        void wait() override {}
        void stopPresenting() noexcept override {}
        void requestClose() noexcept override
        {
            project_->requestClose();
            const auto status = project_->advanceClose();
            assert(status && !*status);
            publication_.retained = true;
            auto completion = stdexec::then(stdexec::schedule(runtime_->main()),
                                            [this]() noexcept
                                            {
                                                assert(project_->manifest().name == "Installed component");
                                                publication_.reservation = {};
                                                publication_.released = true;
                                            });
            auto errors = stdexec::upon_error(std::move(completion), [](lux::process::EExecutionError) noexcept
                                              { assert(false && "Main close completion was rejected"); });
            assert(publication_.tasks.start(std::move(errors)));
            closed_ = true;
        }
        CloseStatus closeStatus() const override
        {
            return {closed_ ? ECloseState::CLOSED : ECloseState::OPEN};
        }

      private:
        ClosingPublication &publication_;
        lux::process::ExecutionRuntime *runtime_{};
        Project *project_{};
        bool closed_{};
    };
} // namespace

int sceneWorkflow(const std::filesystem::path &root, bool measure)
{
    createProject(root);
    lux::meta::ReflectionRegistry::initRegistry();
    Evidence evidence;
    evidence.measure = measure;
    lux::editor::EditorConfig config;
    config.project_file = root / "Project.luxproject";
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.frontend = [&] { return std::make_unique<Probe>(evidence); };
    lux::editor::Editor editor(std::move(config));
    const auto result = editor.exec();
    if (result || !evidence.complete || !evidence.closed)
    {
        if (!editor.outcome())
        {
            std::fprintf(stderr, "FAIL %s:%llu %s\n", editor.outcome().error().domain.c_str(),
                         editor.outcome().error().reason, editor.outcome().error().message.c_str());
        }
        return 1;
    }
    std::printf("PASS installed generated Scene Inspector: draws=%zu, map edit, Undo/Redo, fixed capture save, reopen, "
                "normal close\n",
                evidence.draws);

    ClosingPublication publication;
    lux::editor::EditorConfig closing;
    closing.project_file = root / "Project.luxproject";
    closing.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    closing.frontend = [&] { return std::make_unique<ClosingFrontend>(publication); };
    lux::editor::Editor close_editor(std::move(closing));
    assert(close_editor.exec() == 0);
    assert(publication.retained && publication.released);
    assert(stdexec::sync_wait(publication.tasks.close()));
    std::puts("PASS Project close: incomplete result retained owner until queued Main publication release");

    const auto indexed_root = root / "indexed";
    createProject(indexed_root, true);
    const auto original_source = projectFileDigest(indexed_root / "Main.luxscene");
    assert(original_source);
    Evidence indexed;
    indexed.indexed = true;
    EditorConfig indexed_config;
    indexed_config.project_file = indexed_root / "Project.luxproject";
    indexed_config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    indexed_config.frontend = [&] { return std::make_unique<Probe>(indexed); };
    Editor indexed_editor(std::move(indexed_config));
    assert(indexed_editor.exec() == 0 && indexed.complete && indexed.closed);
    const auto retained_source = projectFileDigest(indexed_root / "Main.luxscene");
    assert(retained_source && *retained_source == *original_source);
    return 0;
}
