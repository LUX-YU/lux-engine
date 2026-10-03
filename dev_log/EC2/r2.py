from pathlib import Path
import re

s = Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p = Path(__file__).parent / 'pending'
def put(name, text):
    target = s / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text, encoding='utf-8', newline='\n')
def change(name, old, new):
    text = (s / name).read_text()
    assert old in text, (name, old[:80])
    put(name, text.replace(old, new))
for name in ['SceneConfigurationDraft.hpp', 'SceneConfigurationDraft.cpp']:
    suffix = 'include/lux/engine/editor/scene/' if name.endswith('hpp') else 'src/'
    put('editor/authoring/scene/' + suffix + name, (p / name).read_text())
for name in ['SceneConfigurationPreparation.hpp', 'SceneConfigurationPreparation.cpp']:
    suffix = 'include/lux/engine/editor/scene/' if name.endswith('hpp') else 'src/'
    put('editor/activities/scene/' + suffix + name, (p / name).read_text())

header = 'editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp'
t = (s / header).read_text()
t = t.replace('#include <lux/engine/scene/SceneDescriptionBuilder.hpp>',
    '#include <lux/engine/editor/scene/SceneConfigurationPreparation.hpp>')
t = t.replace('            std::any\n', '            std::any,\n            ScenePreparationFailure\n')
t = re.sub(r'    struct SceneProviderOption final.*?    enum class ESceneConfigurationStage',
    '    enum class ESceneConfigurationStage', t, flags=re.S)
t = re.sub(r'    enum class ESceneContentPreset.*?    // Owns only', '    // Owns only', t, flags=re.S)
t = t.replace('        // An empty control', '        // An empty control')
t = t.replace('        std::span<const lux::scene::RenderFeatureSceneBinding> feature_bindings{};', '''        std::span<const lux::scene::RenderFeatureSceneBinding> feature_bindings{};
        [[nodiscard]] SceneConfigurationRegistrations registrations() const noexcept
        {
            return {catalog, components, simulation_systems, scene_systems, features, providers, feature_bindings};
        }''')
t = t.replace('        [[nodiscard]] SceneConfigurationResult<SceneCreationConfiguration> build();', '''        [[nodiscard]] SceneConfigurationResult<SceneConfigurationDraft> capture();
        [[nodiscard]] SceneConfigurationResult<SceneCreationConfiguration> build();''')
t = t.replace('system::SystemInstanceId viewport = {}\n', 'system::SystemInstanceId viewport = {},\n            sessions::ContentStamp based_on = {}\n')
put(header, t)

file = 'editor/workbench/scene/src/SceneConfigurationElement.cpp'
t = (s / file).read_text()
anchor = '        using Inputs = SceneConfigurationInputs;'
t = t.replace(anchor, anchor + '''
        SceneConfigurationFailure presentationFailure(ScenePreparationFailure failure)
        {
            const auto code = failure.code == EScenePreparationError::MISSING_PROVIDER
                ? ESceneConfigurationError::MISSING_PROVIDER : ESceneConfigurationError::INVALID_ARGUMENT;
            return {code, failure.domain, failure.reason, failure.message, std::move(failure)};
        }
        template<class T> SceneConfigurationResult<T> presentationResult(ScenePreparationResult<T> result)
        {
            if (!result)
                return cxx::unexpected(presentationFailure(std::move(result.error())));
            return std::move(*result);
        }''')
# Dependency and page-size validation is authoritative in the preparation function.
a = t.index('                    const bool is_nonfinite_page_size')
b = t.index('                    for (const auto& feature : features_)\n                    {\n                        if (!feature->enabled.value())\n                            continue;\n                        const auto& selected', a)
t = t[:a] + t[b:]
a = t.index('            SceneConfigurationResult<void> bind(')
b = t.index('            template <class System> SceneConfigurationResult<void> load', a)
t = t[:a] + '''            SceneConfigurationResult<SceneSystemConfigurationDraft> capture() const
            {
                auto encoded = encode();
                if (!encoded)
                    return cxx::unexpected(std::move(encoded.error()));
                const auto& type = description();
                SceneSystemConfigurationDraft result{
                    simulation ? EConfigurationSystemDomain::SIMULATION : EConfigurationSystemDomain::SCENE,
                    id, name(), simulation ? simulation->type : scene->type, type.version,
                    std::string(type.configuration_schema_name), type.configuration_schema_version,
                    std::move(*encoded), {}
                };
                for (const auto& field : providers_)
                {
                    const auto index = field->choice.value();
                    const bool is_selected = index > 0 && static_cast<std::size_t>(index) < field->names.size();
                    if (is_selected)
                        result.providers.push_back({field->requirement, field->names[index]});
                }
                return result;
            }
''' + t[b:]
a = t.index('            template <class System> SceneConfigurationResult<void> load')
b = t.index('            void setStage', a)
load = t[a:b].replace('template <class System> ', '').replace('const System& source', 'const SceneSystemConfigurationDraft& source')
for old,new in [('version()', 'version'), ('configurationSchemaName()', 'configuration_schema'),
    ('configurationSchemaVersion()', 'configuration_version'), ('instanceName()', 'name'),
    ('configurationPayload()', 'configuration')]:
    load = load.replace('source.' + old, 'source.' + new)
load = load.replace('const auto payload = source.configuration;', 'const std::span<const std::byte> payload = source.configuration;')
a1 = load.index('                if constexpr (requires { source.requirementBindingCount(); })')
load = load[:a1] + '''                for (const auto& binding : source.providers)
                {
                    auto field = std::ranges::find_if(providers_, [&](const auto& item) {
                        return item->requirement == binding.requirement;
                    });
                    if (field == providers_.end())
                        return cxx::unexpected(SceneConfigurationFailure{
                            ESceneConfigurationError::MISSING_PROVIDER, "scene.configuration.requirement"
                        });
                    auto& names = (*field)->names;
                    auto provider = std::ranges::find(names, binding.provider);
                    if (provider == names.end())
                    {
                        // Retain unavailable providers visibly rather than rebinding to a current first match.
                        names.push_back(binding.provider);
                        std::vector<controls::ChoiceOption> options;
                        for (std::size_t index{}; index < names.size(); ++index)
                            options.push_back({static_cast<std::int64_t>(index), names[index]});
                        (*field)->choice.setOptions(std::move(options));
                        (*field)->choice.setValue(names.size() - 1);
                    }
                    else
                        (*field)->choice.setValue(provider - names.begin());
                }
                return {};
            }
'''
t = t[:a] + load + t[b:]
a = t.index('            void presetBindings()')
b = t.index('            SceneConfigurationResult<void> checkFeatures(', a)
t = t[:a] + t[b:]
a = t.index('            SceneConfigurationResult<void> presetFeatures(')
b = t.index('            SystemId id;', a)
t = t[:a] + t[b:]
# Window uses explicit partition from the current owned draft.
t = t.replace('ESceneConfigurationStage stage)\n        {\n            std::vector<controls::ChoiceOption>',
    'ESceneConfigurationStage stage, std::string_view partition)\n        {\n            std::vector<controls::ChoiceOption>')
t = t.replace('"lux.spatial.builtin.single"\n            );', 'partition\n            );')
t = t.replace('ESceneConfigurationStage stage\n        )\n        {\n            auto options = systemOptions(inputs, stage);',
    'ESceneConfigurationStage stage, std::string_view partition = "lux.spatial.builtin.single"\n        )\n        {\n            auto options = systemOptions(inputs, stage, partition);')
t = t.replace('makeSystemChoice(add_row_, inputs_, stage)', 'makeSystemChoice(add_row_, inputs_, stage, origin_.partition)')
t = t.replace('if (base_)\n                return authoringFacts(base_->world->data(), inputs_.components);',
    'if (origin_.base)\n                return authoringFacts(origin_.base->world->data(), inputs_.components, origin_.based_on);')
t = t.replace('return {{}, selected_schemas_, inputs_.components, "lux.spatial.builtin.single", 1};',
    'return {origin_.based_on, selected_schemas_, inputs_.components, origin_.partition, origin_.partition_version};')
a = t.index('        template <class Failure>\n        SceneConfigurationResult<SceneCreationConfiguration> rejected')
b = t.index('        AuthoringFacts facts()', a)
t = t[:a] + t[b:]
a = t.index('        SceneConfigurationResult<SceneCreationConfiguration> build()')
b = t.index('        void update() noexcept', a)
t = t[:a] + '''        SceneConfigurationResult<SceneConfigurationDraft> capture()
        {
            finishControls(owner_);
            auto draft = origin_;
            draft.name = name_.value();
            if (!draft.base)
            {
                draft.schemas.clear();
                for (const auto& [id, field] : schema_fields_)
                    if (field->value())
                        draft.schemas.push_back(id);
            }
            draft.systems = opaque_systems_;
            for (const auto& row : rows_)
            {
                auto value = row->capture();
                if (!value)
                    return cxx::unexpected(std::move(value.error()));
                draft.systems.push_back(std::move(*value));
            }
            draft.construction = construction_;
            draft.scene_dependencies = scene_dependencies_;
            draft.execution = execution_;
            draft.producers = producers_;
            draft.viewport = viewport_;
            return draft;
        }
        SceneConfigurationResult<SceneCreationConfiguration> build()
        {
            auto draft = capture();
            if (!draft)
                return cxx::unexpected(std::move(draft.error()));
            return presentationResult(prepareSceneConfiguration(*draft, inputs_.registrations()));
        }
        SceneConfigurationResult<void> loadDraft(SceneConfigurationDraft draft)
        {
            origin_ = std::move(draft);
            for (auto& [id, field] : schema_fields_)
                field->setValue(std::ranges::find(origin_.schemas, id) != origin_.schemas.end());
            name_.setValue(origin_.name);
            rows_.clear();
            opaque_systems_.clear();
            construction_ = origin_.construction;
            scene_dependencies_ = origin_.scene_dependencies;
            execution_ = origin_.execution;
            producers_ = origin_.producers;
            viewport_ = origin_.viewport;
            next_system_ = 1;
            SceneConfigurationResult<void> status;
            for (const auto& source : origin_.systems)
            {
                if (source.id.value == UINT64_MAX)
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.identity.exhausted"
                    });
                next_system_ = std::max(next_system_, source.id.value + 1);
                const simulation::SimulationSystemRegistration* sim{};
                const lux::scene::SceneSystemRegistration* scene{};
                if (source.domain == EConfigurationSystemDomain::SIMULATION)
                    sim = inputs_.simulation_systems.find(source.type);
                else
                {
                    const auto found = std::ranges::find(inputs_.scene_systems, source.type,
                        &lux::scene::SceneSystemRegistration::type);
                    if (found != inputs_.scene_systems.end())
                        scene = &*found;
                }
                const auto* type = sim ? &sim->description->type : scene ? scene->description : nullptr;
                const bool is_known = type && source.version == type->version &&
                    source.configuration_schema == type->configuration_schema_name &&
                    source.configuration_version == type->configuration_schema_version;
                if (!is_known)
                {
                    opaque_systems_.push_back(source);
                    continue;
                }
                auto row = std::make_unique<SystemElement>(systems_, source.id, inputs_, sim, scene, status);
                if (!status)
                    return status;
                auto loaded = row->load(source, inputs_);
                if (!loaded)
                    return loaded;
                rows_.push_back(std::move(row));
            }
            schemas_.setEnabled(!origin_.base);
            name_.setEnabled(!origin_.base);
            note_.setText(origin_.partition + (opaque_systems_.empty() ? "" :
                " — unavailable system configurations are retained without changes"));
            rebuildEndpoints();
            setStage(stage_);
            return {};
        }
        SceneConfigurationResult<void> load(
            const SceneConfiguration& value, SystemId viewport, sessions::ContentStamp based_on
        )
        {
            auto draft = captureSceneConfiguration(value, based_on, viewport);
            if (!draft)
                return cxx::unexpected(SceneConfigurationFailure{
                    ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.input", 0, {}, draft.error()
                });
            return loadDraft(std::move(*draft));
        }
''' + t[b:]
a = t.index('        SceneConfigurationResult<void> applyPreset(')
b = t.index('        SceneConfigurationElement& owner_;', a)
t = t[:a] + '''        SceneConfigurationResult<void> applyPreset(ESceneContentPreset preset)
        {
            finishControls(owner_);
            auto draft = makeSceneConfigurationPreset(preset, "lux.spatial.builtin.single", 1, inputs_.registrations());
            if (!draft)
                return cxx::unexpected(presentationFailure(std::move(draft.error())));
            return loadDraft(std::move(*draft));
        }
        SceneConfigurationDraft origin_{{}, {}, "Untitled scene", "lux.spatial.builtin.single", 1};
        std::vector<SceneSystemConfigurationDraft> opaque_systems_;
''' + t[b:]
t = t.replace('    SceneConfigurationResult<SceneCreationConfiguration> SceneConfigurationElement::build()', '''    SceneConfigurationResult<SceneConfigurationDraft> SceneConfigurationElement::capture()
    {
        return impl_->capture();
    }
    SceneConfigurationResult<SceneCreationConfiguration> SceneConfigurationElement::build()''')
t = t.replace('SceneConfigurationElement::load(const SceneConfiguration& value, SystemId viewport)',
    'SceneConfigurationElement::load(\n        const SceneConfiguration& value, SystemId viewport, sessions::ContentStamp based_on\n    )')
t = t.replace('return impl_->load(value, viewport);', 'return impl_->load(value, viewport, based_on);')
a = t.index('        if (!base.scene', t.index('SceneConfigurationElement::buildEdit'))
b = t.index('    void SceneConfigurationElement::setStage', a)
t = t[:a] + '''        auto draft = impl_->capture();
        if (!draft)
            return cxx::unexpected(std::move(draft.error()));
        const bool is_same_base = draft->base && draft->base->scene == base.scene &&
            draft->base->world == base.world && draft->base->simulation == base.simulation;
        if (!is_same_base)
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.source"
            });
        auto prepared = prepareSceneConfigurationEdit(*draft, impl_->inputs_.registrations());
        if (!prepared)
            return cxx::unexpected(presentationFailure(std::move(prepared.error())));
        return SceneSetConfiguration{std::move(*prepared)};
    }
''' + t[b:]
put(file, t)
change('editor/workbench/scene/src/SceneConfigurationView.cpp',
    'candidate->load(source.configuration());', 'candidate->load(source.configuration(), {}, stamp);')
change('editor/authoring/scene/CMakeLists.txt', 'SOURCE_FILES src/AuthoringFacts.cpp',
    'SOURCE_FILES src/SceneConfigurationDraft.cpp src/AuthoringFacts.cpp')
change('editor/activities/scene/CMakeLists.txt',
    'STATIC SOURCE_FILES src/RunStore.cpp src/ModelCreationOperation.cpp)',
    'STATIC SOURCE_FILES src/SceneConfigurationPreparation.cpp src/RunStore.cpp src/ModelCreationOperation.cpp)')
change('editor/activities/scene/CMakeLists.txt',
    '    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ModelCreationOperation.hpp',
    '    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneConfigurationPreparation.hpp\n    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/ModelCreationOperation.hpp')
