#include <lux/engine/editor/metadata/EngineMetadata.hpp>
#include <lux/engine/editor/metadata/PluginLibrary.hpp>
#include <lux/engine/editor/metadata/ConfigurationValue.hpp>
#include <lux/engine/simulation/Simulation.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/dynamic_library/LibraryExport.hpp>

int main(int argc, char **argv)
{
    assert(argc == 3);
    using namespace lux;
    using Clock = std::chrono::steady_clock;
    const auto microseconds = [](auto elapsed) {
        return std::chrono::duration<double, std::micro>(elapsed).count();
    };
    assert(!meta::ReflectionRegistry::initialized());
    editor::EngineMetadata metadata;
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
        editor::EngineMetadata candidate;
        const auto changed = candidate.read(file, argv[2]);
        assert(!changed && changed.error().code == editor::EPluginError::DECLARATION_MISMATCH);
        assert(candidate.plugins().empty() && !meta::ReflectionRegistry::initialized());
        assert(std::filesystem::remove(file));
    }
    const auto *description = metadata.find("lux.builtin.physics2d");
    assert(description && description->components.size() == 2);
    assert(metadata.implementations("lux.physics2d.query").size() == 1);
    auto order = metadata.loadOrder(description->identity.id);
    assert(order && order->size() == 3);
    const auto load_started = Clock::now();
    std::vector<std::shared_ptr<const editor::PluginLibrary>> dependencies;
    for (const auto *item : *order)
    {
        if (item->identity == description->identity) continue;
        auto dependency = editor::PluginLibrary::load(*item, dependencies);
        assert(dependency);
        dependencies.push_back(std::move(*dependency));
    }
    auto loaded = editor::PluginLibrary::load(*description, dependencies);
    assert(loaded && (*loaded)->simulationSystems().size() == 1 && (*loaded)->components().size() == 2);
    const auto load_finished = Clock::now();
    std::printf("Plugin cold path: directory=%.1f us, load+validate=%.1f us, selected runtime modules=%zu\n",
        microseconds(read_finished - read_started), microseconds(load_finished - load_started), order->size());
    // Neither enumeration nor runtime export loading needs the global tool registry.
    assert(!meta::ReflectionRegistry::initialized());
    auto different_sdk = *description;
    {
        engine::platform::DynamicLibrary original(description->root / description->runtime_library.path,
            engine::platform::ELoadMode::INSTALLED_PLUGIN);
        assert(original.is_loaded());
        auto moved = std::move(original);
        assert(!original.is_loaded() && moved.is_loaded());
        const auto identity = moved.get_symbol<engine::platform::GetLibraryExportIdentity>(
            engine::platform::kLibraryIdentitySymbol);
        assert(identity && identity()->module_id == description->identity.id);
    }
    const auto rejected = [&](auto change, editor::EPluginError expected) {
        auto input = *description;
        change(input);
        const auto result = editor::PluginLibrary::load(input, dependencies);
        assert(!result && result.error().code == expected);
    };
    rejected([](auto &d) { d.runtime_library.path = "bin/absent-test-plugin.dll"; },
             editor::EPluginError::LIBRARY_LOAD_FAILURE);
    rejected([](auto &d) { d.identity.id = "test.wrong.identity"; }, editor::EPluginError::MODULE_MISMATCH);
    rejected([](auto &d) { d.runtime_library.declaration_digest.assign(64, '0'); },
             editor::EPluginError::DECLARATION_MISMATCH);
    rejected([](auto &d) { d.runtime_library.exports.push_back(editor::EPluginExport::RENDER); },
             editor::EPluginError::MISSING_EXPORT);
    different_sdk.runtime_library.sdk_abi = "different-sdk";
    auto wrong_abi = editor::PluginLibrary::load(different_sdk, dependencies);
    assert(!wrong_abi && wrong_abi.error().code == editor::EPluginError::ABI_MISMATCH);
    auto wrong_build_description = *description;
    wrong_build_description.runtime_library.build_id.assign(64, '0');
    auto wrong_build = editor::PluginLibrary::load(wrong_build_description, dependencies);
    assert(!wrong_build && wrong_build.error().code == editor::EPluginError::BUILD_MISMATCH);
    assert(!meta::ReflectionRegistry::initialized());
    std::weak_ptr<const void> code = (*loaded)->runtimeCode();
    {
        auto schemas = simulation::ecs::ComponentSchemaSet::build(
            {(*loaded)->components().begin(), (*loaded)->components().end()});
        assert(schemas);
        simulation::ecs::Registry registry;
        simulation::SimulationSystemRegistry systems;
        assert(systems.add((*loaded)->simulationSystems()));
        const auto &registration = (*loaded)->simulationSystems().front();
        std::vector<std::byte> configuration;
        assert(registration.configuration.encode_default(configuration));
        simulation::SimulationDescriptionBuilder builder;
        assert(builder.addSystem({1}, "physics", *registration.description, configuration));
        auto description_value = std::move(builder).build();
        assert(description_value);
        auto simulation = simulation::Simulation::create(
            registry, std::make_shared<const simulation::SimulationDescription>(std::move(*description_value)), systems);
        assert(simulation && simulation->scriptApiCapabilities().size() == 1);
        assert(simulation->seal());
        loaded->reset();
        assert(!code.expired());
        auto executor = task::TaskExecutor::create({0, 32});
        assert(executor);
        assert(simulation->execute(*executor, std::chrono::milliseconds(17)));
        simulation->stop();
    }
    assert(code.expired());
    assert(!meta::ReflectionRegistry::initialized());
    if (description->editor_library)
    {
        meta::ReflectionRegistry::initRegistry();
        auto library = editor::PluginLibrary::load(*description, dependencies);
        assert(library && (*library)->editorExports());
        const auto *exports = (*library)->editorExports();
        assert(exports->configuration_count == 1);
        const auto &configuration = exports->configurations[0];
        assert(!configuration.reflection(meta::ReflectionRegistry::instance()));
        {
            auto discarded = meta::ReflectionRegistry::beginDraft();
            assert(discarded.append(exports->register_types, (*library)->editorCode()));
            assert(discarded.prepareCommit());
        }
        assert(!configuration.reflection(meta::ReflectionRegistry::instance()));
        auto draft = meta::ReflectionRegistry::beginDraft();
        const auto adoption_started = Clock::now();
        assert(draft.append(exports->register_types, (*library)->editorCode()));
        assert(draft.commit());
        std::printf("Plugin tool registration+adoption=%.1f us\n", microseconds(Clock::now() - adoption_started));
        std::weak_ptr<const void> editor_code = (*library)->editorCode();
        {
            auto value = editor::ConfigurationValue::create(configuration, (*library)->editorCode());
            assert(value);
            std::vector<std::byte> bytes;
            assert(value->encode(bytes) && !bytes.empty());
            assert(value->decode(bytes));
            auto duplicate = meta::ReflectionRegistry::beginDraft();
            assert(!duplicate.append(exports->register_types, (*library)->editorCode()));
            library->reset();
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
