#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/editor/metadata/EditorPlugin.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <cassert>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryExport.hpp>

int main(int argc, char** argv)
{
    assert(argc == 3);
    using namespace lux;
    using Clock = std::chrono::steady_clock;
    const auto microseconds = [](auto elapsed) { return std::chrono::duration<double, std::micro>(elapsed).count(); };
    assert(!meta::ReflectionRegistry::initialized());
    project::PluginCatalog metadata;
    const auto read_started = Clock::now();
    assert(metadata.read(argv[1], argv[2]));
    const auto read_finished = Clock::now();
    {
        std::ifstream original(argv[1], std::ios::binary);
        std::string altered{std::istreambuf_iterator<char>{original}, std::istreambuf_iterator<char>{}};
        constexpr std::string_view original_id = "\"id\": \"lux.physics2d.BoxCollider\"";
        const auto position = altered.find(original_id);
        assert(position != std::string::npos);
        altered.replace(position, original_id.size(), "\"id\": \"test.changed.Contract\"");
        const auto file = std::filesystem::path(argv[1]).concat(".changed.json");
        {
            std::ofstream output(file, std::ios::binary);
            output << altered;
            assert(output.good());
        }
        project::PluginCatalog candidate;
        const auto changed = candidate.read(file, argv[2]);
        assert(!changed && changed.error().code == project::EPluginError::DECLARATION_MISMATCH);
        assert(candidate.plugins().empty() && !meta::ReflectionRegistry::initialized());
        assert(std::filesystem::remove(file));
    }
    const auto* description = metadata.find("lux.builtin.physics2d");
    assert(description && description->components.size() == 2);
    assert(description->description_file == std::filesystem::canonical(argv[1]));
    assert(metadata.implementations("lux.physics2d.query").size() == 1);
    auto order = metadata.loadOrder(description->identity.id);
    assert(order && order->size() == 2);
    const auto load_started = Clock::now();
    std::vector<std::shared_ptr<const project::PluginLibrary>> dependencies;
    for (const auto* item : *order)
    {
        if (item->identity == description->identity)
            continue;
        auto dependency = project::PluginLibrary::load(*item, dependencies);
        assert(dependency);
        dependencies.push_back(std::move(*dependency));
    }
    auto loaded = project::PluginLibrary::load(*description, dependencies);
    assert(loaded && (*loaded)->simulationSystems().size() == 1 && (*loaded)->components().size() == 2);
    const auto load_finished = Clock::now();
    std::printf(
        "Plugin cold path: directory=%.1f us, load+validate=%.1f us, selected runtime modules=%zu\n",
        microseconds(read_finished - read_started),
        microseconds(load_finished - load_started),
        order->size()
    );
    // Neither enumeration nor runtime export loading needs the global tool registry.
    assert(!meta::ReflectionRegistry::initialized());
    auto different_sdk = *description;
    {
        engine::platform::DynamicLibrary original(
            description->root / description->runtime_library.path,
            engine::platform::ELoadMode::INSTALLED_PLUGIN
        );
        assert(original.is_loaded());
        auto moved = std::move(original);
        assert(!original.is_loaded() && moved.is_loaded());
        const auto identity =
            moved.get_symbol<engine::platform::GetLibraryExportIdentity>(engine::platform::kLibraryIdentitySymbol);
        assert(identity && identity()->module_id == description->identity.id);
    }
    const auto rejected = [&](auto change, project::EPluginError expected) {
        auto input = *description;
        change(input);
        const auto result = project::PluginLibrary::load(input, dependencies);
        assert(!result && result.error().code == expected);
    };
    rejected(
        [](auto& d) { d.runtime_library.path = "bin/absent-test-plugin.dll"; },
        project::EPluginError::LIBRARY_LOAD_FAILURE
    );
    rejected([](auto& d) { d.identity.id = "test.wrong.identity"; }, project::EPluginError::MODULE_MISMATCH);
    rejected(
        [](auto& d) { d.runtime_library.declaration_digest.assign(64, '0'); },
        project::EPluginError::DECLARATION_MISMATCH
    );
    rejected(
        [](auto& d) { d.runtime_library.exports.push_back(project::EPluginExport::SCENE); },
        project::EPluginError::MISSING_EXPORT
    );
    different_sdk.runtime_library.sdk_abi = "different-sdk";
    auto wrong_abi = project::PluginLibrary::load(different_sdk, dependencies);
    assert(!wrong_abi && wrong_abi.error().code == project::EPluginError::ABI_MISMATCH);
    auto wrong_build_description = *description;
    wrong_build_description.runtime_library.build_id.assign(64, '0');
    auto wrong_build = project::PluginLibrary::load(wrong_build_description, dependencies);
    assert(!wrong_build && wrong_build.error().code == project::EPluginError::BUILD_MISMATCH);
    assert(!meta::ReflectionRegistry::initialized());
    std::weak_ptr<const void> code = (*loaded)->runtimeCode();
    {
        auto schemas =
            simulation::ecs::ComponentSchemaSet::build({(*loaded)->components().begin(), (*loaded)->components().end()}
            );
        assert(schemas);
        simulation::ecs::Registry registry;
        simulation::SimulationSystemRegistry systems;
        assert(systems.add((*loaded)->simulationSystems()));
        const auto& registration = (*loaded)->simulationSystems().front();
        std::vector<std::byte> configuration;
        assert(registration.configuration.encode_default(configuration));
        simulation::SimulationDescriptionBuilder builder;
        assert(builder.addSystem({1}, "physics", *registration.description, configuration));
        auto description_value = std::move(builder).build();
        assert(description_value);
        auto simulation = simulation::Simulation::create(
            registry,
            std::make_shared<const simulation::SimulationDescription>(std::move(*description_value)),
            systems
        );
        assert(simulation && simulation->scriptApiCapabilities().size() == 1);
        assert(simulation->seal());
        loaded->reset();
        assert(!code.expired());
        auto executor = task::TaskExecutor::create({0, 32});
        assert(executor);
        assert(simulation->execute(*executor, {std::chrono::milliseconds(17), std::chrono::milliseconds(17), 1}));
        simulation->stop();
    }
    assert(code.expired());
    assert(!meta::ReflectionRegistry::initialized());
    // Runtime loading never opens the optional Editor binary.
    {
        auto runtime_only = *description;
        if (runtime_only.editor_library)
            runtime_only.editor_library->path = "missing-editor-library.dll";
        assert(project::PluginLibrary::load(runtime_only, dependencies));
    }
    {
        project::PluginCatalog catalog;
        assert(catalog.read(argv[1], argv[2]));
        const std::array selected{description->identity};
        auto manager = project::PluginManager::create(std::move(catalog), selected);
        assert(manager && manager->find(description->identity.id));
        assert(manager->libraries().size() == order->size());
    }
    const auto reject_selection = [&](std::vector<project::MetadataIdentity> selection,
                                      project::EPluginError expected) {
        project::PluginCatalog catalog;
        assert(catalog.read(argv[1], argv[2]));
        auto result = project::PluginManager::create(std::move(catalog), selection);
        assert(!result && result.error().code == expected);
    };
    reject_selection({description->identity, description->identity}, project::EPluginError::DUPLICATE_IDENTITY);
    reject_selection({{"test.absent.plugin", 1}}, project::EPluginError::MISSING_DEPENDENCY);
    reject_selection(
        {{description->identity.id, description->identity.version + 1}},
        project::EPluginError::DEPENDENCY_VERSION_MISMATCH
    );
    if (description->editor_library)
    {
        meta::ReflectionRegistry::initRegistry();
        std::vector<editor::EditorPlugin> editor_dependencies;
        for (const auto* dependency : *order)
        {
            if (dependency->identity == description->identity)
                continue;
            const auto runtime = std::ranges::find_if(dependencies, [&](const auto& value) {
                return value->identity() == dependency->identity;
            });
            assert(runtime != dependencies.end());
            auto extension = editor::loadEditorPlugin(*dependency, **runtime, editor_dependencies);
            assert(extension);
            editor_dependencies.push_back(std::move(*extension));
        }
        auto runtime = project::PluginLibrary::load(*description, dependencies);
        assert(runtime);
        auto library = editor::loadEditorPlugin(*description, **runtime, editor_dependencies);
        assert(library && library->exports);
        const auto* exports = library->exports;
        assert(exports->configuration_count == 1);
        const auto& configuration = exports->configurations[0];
        assert(!configuration.reflection(meta::ReflectionRegistry::instance()));
        {
            auto discarded = meta::ReflectionRegistry::beginDraft();
            assert(discarded.append(exports->register_types, library->code));
            assert(discarded.prepareCommit());
        }
        assert(!configuration.reflection(meta::ReflectionRegistry::instance()));
        auto draft = meta::ReflectionRegistry::beginDraft();
        const auto adoption_started = Clock::now();
        assert(draft.append(exports->register_types, library->code));
        assert(draft.commit());
        std::printf("Plugin tool registration+adoption=%.1f us\n", microseconds(Clock::now() - adoption_started));
        std::weak_ptr<const void> editor_code = library->code;
        {
            auto value = editor::ConfigurationValue::create(configuration, library->code);
            assert(value);
            std::vector<std::byte> bytes;
            assert(value->encode(bytes) && !bytes.empty());
            assert(value->decode(bytes));
            auto duplicate = meta::ReflectionRegistry::beginDraft();
            assert(!duplicate.append(exports->register_types, library->code));
            library->code.reset();
            assert(!editor_code.expired());
        }
        assert(!editor_code.expired());
        meta::ReflectionRegistry::destroyRegistry();
        assert(editor_code.expired());
    }
    // Duplicate descriptions do not mutate the original directory.
    const auto count = metadata.plugins().size();
    assert(!metadata.read(argv[1], argv[2]));
    assert(metadata.plugins().size() == count);
}
