#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace lux::editor::scene
{
    namespace
    {
        namespace controls = lux::ui;
        using SystemId = lux::system::SystemInstanceId;
        void finishControls(object::LuxObject& owner)
        {
            if (auto* element = dynamic_cast<controls::Element*>(&owner))
                element->finishEdit();
            for (auto* child = owner.firstChild(); child; child = child->nextSibling())
                finishControls(*child);
        }
        using Inputs = SceneConfigurationInputs;
        object::Connection takeConnection(
            object::LuxObject::ConnectResult connected,
            SceneConfigurationResult<void>& status
        )
        {
            if (connected)
                return std::move(*connected);
            if (connected.error() == object::EConnectError::ALLOCATION_FAILURE)
                std::terminate();
            status = cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::CONTROL_FAILURE,
                "scene.configuration.connect",
                0,
                {},
                connected.error()
            });
            return {};
        }
        std::string origin(const Inputs& inputs, std::string_view type)
        {
            for (const auto& plugin : inputs.catalog.plugins())
                for (const auto& system : plugin.systems)
                    if (system.identity.id == type)
                        return " [" + std::string(plugin.builtin ? "Builtin: " : "Plugin: ") + plugin.identity.id + "]";
            return {};
        }
        struct NumberField final
        {
            controls::Label label;
            controls::NumericEdit edit;
            NumberField(controls::Element& parent, std::string name, controls::VNumericValue value)
                : label(parent, controls::ElementId{name + "-label"}, name),
                  edit(parent, controls::ElementId{name}, value)
            {}
        };
        struct ProviderField final
        {
            std::string requirement;
            bool required;
            std::vector<std::string> names;
            controls::Label label;
            controls::Choice choice;
            ProviderField(
                controls::Element& parent,
                const lux::scene::SceneSystemRequirementSpec& spec,
                const Inputs& inputs
            )
                : requirement(spec.name), required(!spec.optional),
                  label(parent, controls::ElementId{requirement + "-label"}, requirement),
                  choice(parent, controls::ElementId{requirement}, options(spec, names, inputs), 0)
            {}
            static std::vector<controls::ChoiceOption> options(
                const lux::scene::SceneSystemRequirementSpec& spec,
                std::vector<std::string>& names,
                const Inputs& inputs
            )
            {
                names = {""};
                for (const auto& provider : inputs.providers)
                    if (provider.capability == spec.capability)
                        names.emplace_back(provider.name);
                std::vector<controls::ChoiceOption> result{
                    {0, spec.optional ? "Not bound" : "Select provider (required)"}
                };
                for (std::size_t i = 1; i < names.size(); ++i)
                    result.push_back({static_cast<std::int64_t>(i), names[i]});
                return result;
            }
        };
        struct ConfigurationField final
        {
            ConfigurationControl control;
            std::optional<std::vector<std::byte>> original;
            serialization::SerializationResult encode(
                const serialization::PortableValueCodec& codec,
                std::vector<std::byte>& bytes
            ) const noexcept
            {
                if (control)
                    return control.encode(bytes);
                if (original)
                {
                    bytes = *original;
                    return {};
                }
                return codec.encode_default(bytes);
            }
            SceneConfigurationResult<void> create(
                controls::Element& parent,
                std::string_view schema,
                std::uint32_t version,
                const serialization::PortableValueCodec& codec,
                const Inputs& inputs,
                std::optional<std::span<const std::byte>> initial = {}
            )
            {
                if (initial)
                    original.emplace(initial->begin(), initial->end());
                if (!inputs.configuration)
                    return {};
                auto created = inputs.configuration(parent, schema, version, codec, initial);
                if (!created)
                    return cxx::unexpected(created.error());
                control = std::move(*created);
                return {};
            }
        };
        struct FeatureField final
        {
            const lux::render::RenderFeatureRegistration& registration;
            controls::Layout layout;
            controls::CheckBox enabled;
            ConfigurationField configuration;
            FeatureField(
                controls::Element& parent,
                const lux::render::RenderFeatureRegistration& feature,
                const Inputs& inputs,
                SceneConfigurationResult<void>& status
            )
                : registration(feature),
                  layout(parent, controls::ElementId{std::string(feature.factory.descriptor.canonical_name)}),
                  enabled(
                      layout,
                      controls::ElementId{"enabled"},
                      std::string(feature.factory.descriptor.canonical_name)
                  )
            {
                status = configuration.create(
                    layout,
                    feature.configuration.schema,
                    feature.configuration.schema_version,
                    feature.configuration.portable,
                    inputs
                );
            }
        };
        class SystemElement final : public controls::Element
        {
        public:
            SystemElement(
                controls::Element& parent,
                SystemId identity,
                const Inputs& inputs,
                const lux::simulation::SimulationSystemRegistration* simulation,
                const lux::scene::SceneSystemRegistration* scene,
                SceneConfigurationResult<void>& status
            )
                : Element(parent, controls::ElementId{"system/" + std::to_string(identity.value)}), id(identity),
                  simulation(simulation), scene(scene), layout_(*this, controls::ElementId{"row"}),
                  title_(
                      layout_,
                      controls::ElementId{"type"},
                      std::string(description().canonical_name) + origin(inputs, description().canonical_name)
                  ),
                  name_(layout_, controls::ElementId{"name"}, "system-" + std::to_string(identity.value)),
                  fields_(layout_, controls::ElementId{"fields"}, controls::ELayoutType::FORM),
                  remove_(layout_, controls::ElementId{"remove"}, "Remove system")
            {
                setStretch({1, 0});
                removed_ = takeConnection(
                    object::LuxObject::connect(
                        &remove_,
                        &controls::Button::activated,
                        [this]() noexcept { remove = true; }
                    ),
                    status
                );
                if (!status)
                    return;
                const auto& codec = simulation ? simulation->configuration : scene->configuration;
                if (codec.type == lux::cxx::typeToken<lux::scene::TransformSystemConfiguration>())
                {
                    numbers_.push_back(std::make_unique<NumberField>(fields_, "Entity capacity", std::uint64_t{4096}));
                    numbers_.push_back(std::make_unique<NumberField>(fields_, "Command capacity", std::uint64_t{8192}));
                    numbers_.push_back(
                        std::make_unique<NumberField>(fields_, "Command payload bytes", std::uint64_t{1048576})
                    );
                }
                else if (codec.type == lux::cxx::typeToken<lux::scene::RenderSystemConfiguration>())
                {
                    numbers_.push_back(std::make_unique<NumberField>(fields_, "Coordinate page size", 1024.0));
                    for (const auto& feature : inputs.features)
                    {
                        if (!feature.scene_configurable)
                            continue;
                        features_.push_back(std::make_unique<FeatureField>(layout_, feature, inputs, status));
                        if (!status)
                            return;
                    }
                }
                else if (codec.type == lux::cxx::typeToken<lux::scene::WorldLoadingConfiguration>())
                    bootstrap_ = std::make_unique<controls::CheckBox>(
                        layout_,
                        controls::ElementId{"bootstrap"},
                        "Retain the single partition when running",
                        true
                    );
                else if (codec.valid())
                    status = configuration_.create(
                        layout_,
                        description().configuration_schema_name,
                        description().configuration_schema_version,
                        codec,
                        inputs
                    );
                if (scene)
                    for (const auto& requirement : scene->requirements)
                        providers_.push_back(std::make_unique<ProviderField>(fields_, requirement, inputs));
            }
            const lux::system::SystemTypeDescription& description() const noexcept
            {
                return simulation ? simulation->description->type : *scene->description;
            }
            const std::string& name() const noexcept
            {
                return name_.value();
            }
            SceneConfigurationResult<std::vector<std::byte>> encode() const
            {
                const auto& codec = simulation ? simulation->configuration : scene->configuration;
                std::vector<std::byte> bytes;
                serialization::SerializationResult encoded;
                if (!codec.valid())
                    return bytes;
                if (configuration_.control || configuration_.original)
                    encoded = configuration_.encode(codec, bytes);
                else if (codec.type == lux::cxx::typeToken<lux::scene::TransformSystemConfiguration>())
                {
                    const lux::scene::TransformSystemConfiguration value{
                        std::get<std::uint64_t>(numbers_[0]->edit.value()),
                        std::get<std::uint64_t>(numbers_[1]->edit.value()),
                        std::get<std::uint64_t>(numbers_[2]->edit.value())
                    };
                    encoded = codec.encode(&value, bytes);
                }
                else if (bootstrap_)
                {
                    lux::scene::WorldLoadingConfiguration value;
                    if (bootstrap_->value())
                        value.bootstrap.push_back({0});
                    encoded = codec.encode(&value, bytes);
                }
                else if (codec.type == lux::cxx::typeToken<lux::scene::RenderSystemConfiguration>())
                {
                    lux::scene::RenderSystemConfiguration value;
                    value.features = unknown_features_;
                    value.coordinate_page_size = std::get<double>(numbers_[0]->edit.value());
                    const bool is_nonfinite_page_size = !std::isfinite(value.coordinate_page_size);
                    const bool is_nonpositive_page_size = value.coordinate_page_size <= 0;
                    const bool is_invalid_page_size = is_nonfinite_page_size || is_nonpositive_page_size;
                    if (is_invalid_page_size)
                        return lux::cxx::unexpected(SceneConfigurationFailure{
                            ESceneConfigurationError::INVALID_ARGUMENT,
                            "scene.new.coordinate_page_size"
                        });
                    // Resolve UI choices against the fixed registrations before publishing a file.
                    // Installation still validates the real device's accepted feature catalog.
                    for (const auto& feature : features_)
                    {
                        if (!feature->enabled.value())
                            continue;
                        const auto& descriptor = feature->registration.factory.descriptor;
                        for (const auto& dependency : descriptor.dependencies)
                        {
                            const auto provider = std::ranges::find_if(features_, [&](const auto& candidate) {
                                return candidate->enabled.value() &&
                                       candidate->registration.factory.descriptor.type == dependency.type;
                            });
                            const bool missing = provider == features_.end();
                            const bool wrong_version =
                                !missing &&
                                (*provider)->registration.factory.descriptor.abi_version != dependency.abi_version;
                            const bool is_missing_required_dependency = missing && !dependency.optional;
                            const bool is_invalid_dependency = is_missing_required_dependency || wrong_version;
                            if (is_invalid_dependency)
                                return lux::cxx::unexpected(SceneConfigurationFailure{
                                    ESceneConfigurationError::INVALID_ARGUMENT,
                                    "scene.new.feature.dependency",
                                    dependency.type,
                                    std::string(descriptor.canonical_name)
                                });
                        }
                        for (const auto conflict : descriptor.conflicts)
                            if (std::ranges::any_of(features_, [&](const auto& candidate) {
                                    return candidate->enabled.value() &&
                                           candidate->registration.factory.descriptor.type == conflict;
                                }))
                                return lux::cxx::unexpected(SceneConfigurationFailure{
                                    ESceneConfigurationError::INVALID_ARGUMENT,
                                    "scene.new.feature.conflict",
                                    conflict,
                                    std::string(descriptor.canonical_name)
                                });
                    }
                    for (const auto& feature : features_)
                    {
                        if (!feature->enabled.value())
                            continue;
                        const auto& selected = feature->registration;
                        std::vector<std::byte> config;
                        auto result = feature->configuration.encode(selected.configuration.portable, config);
                        if (!result)
                            return lux::cxx::unexpected(SceneConfigurationFailure{
                                ESceneConfigurationError::INVALID_ARGUMENT,
                                "scene.new.feature",
                                0,
                                {},
                                result.error()
                            });
                        value.features.push_back(
                            {selected.factory.descriptor.type,
                             std::move(config),
                             std::string(selected.configuration.schema),
                             selected.configuration.schema_version}
                        );
                    }
                    encoded = codec.encode(&value, bytes);
                }
                else
                    encoded = codec.encode_default(bytes);
                if (!encoded)
                    return lux::cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT,
                        "scene.new.configuration",
                        0,
                        {},
                        encoded.error()
                    });
                return bytes;
            }
            SceneConfigurationResult<void> bind(lux::scene::SceneDescriptionBuilder& builder) const
            {
                for (const auto& field : providers_)
                {
                    const auto index = static_cast<std::size_t>(field->choice.value());
                    if (index == 0)
                    {
                        if (field->required)
                            return lux::cxx::unexpected(SceneConfigurationFailure{
                                ESceneConfigurationError::MISSING_PROVIDER,
                                "scene.new.provider",
                                0,
                                "Select a provider for " + field->requirement
                            });
                        continue;
                    }
                    const auto bound = builder.bindRequirement(id, field->requirement, field->names[index]);
                    if (!bound)
                        return lux::cxx::unexpected(SceneConfigurationFailure{
                            ESceneConfigurationError::INVALID_ARGUMENT,
                            "scene.new.provider",
                            0,
                            {},
                            bound.error()
                        });
                }
                return {};
            }
            template <class System> SceneConfigurationResult<void> load(const System& source, const Inputs& inputs)
            {
                const auto& expected = description();
                const bool is_version_mismatch = source.version() != expected.version;
                const bool is_schema_mismatch =
                    source.configurationSchemaName() != expected.configuration_schema_name ||
                    source.configurationSchemaVersion() != expected.configuration_schema_version;
                if (is_version_mismatch || is_schema_mismatch)
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT,
                        "scene.configuration.version"
                    });
                name_.setValue(std::string(source.instanceName()));
                const auto& codec = simulation ? simulation->configuration : scene->configuration;
                const auto payload = source.configurationPayload();
                serialization::SerializationResult decoded;
                if (codec.type == cxx::typeToken<lux::scene::TransformSystemConfiguration>())
                {
                    lux::scene::TransformSystemConfiguration value;
                    decoded = codec.decode(payload, &value);
                    if (decoded)
                    {
                        numbers_[0]->edit.setValue(value.entity_capacity);
                        numbers_[1]->edit.setValue(value.max_commands);
                        numbers_[2]->edit.setValue(value.max_payload_bytes);
                    }
                }
                else if (bootstrap_)
                {
                    lux::scene::WorldLoadingConfiguration value;
                    decoded = codec.decode(payload, &value);
                    if (decoded)
                    {
                        // The checkbox edits the built-in single-partition contract only.
                        const bool is_single = value.bootstrap.empty() ||
                                               (value.bootstrap.size() == 1 && value.bootstrap.front().value == 0);
                        if (!is_single)
                            return cxx::unexpected(SceneConfigurationFailure{
                                ESceneConfigurationError::INVALID_ARGUMENT,
                                "scene.configuration.partition"
                            });
                        bootstrap_->setValue(!value.bootstrap.empty());
                    }
                }
                else if (codec.type == cxx::typeToken<lux::scene::RenderSystemConfiguration>())
                {
                    lux::scene::RenderSystemConfiguration value;
                    decoded = codec.decode(payload, &value);
                    if (decoded)
                    {
                        numbers_[0]->edit.setValue(value.coordinate_page_size);
                        for (const auto& feature : value.features)
                        {
                            auto found = std::ranges::find_if(features_, [&](const auto& item) {
                                return item->registration.factory.descriptor.type == feature.type;
                            });
                            if (found == features_.end())
                            {
                                unknown_features_.push_back(feature);
                                continue;
                            }
                            auto& row = **found;
                            const bool is_feature_schema_mismatch =
                                feature.configuration_schema != row.registration.configuration.schema ||
                                feature.configuration_version != row.registration.configuration.schema_version;
                            if (is_feature_schema_mismatch)
                                return cxx::unexpected(SceneConfigurationFailure{
                                    ESceneConfigurationError::INVALID_ARGUMENT,
                                    "scene.configuration.feature.version"
                                });
                            row.enabled.setValue(true);
                            row.configuration.control = {};
                            auto loaded = row.configuration.create(
                                row.layout,
                                feature.configuration_schema,
                                feature.configuration_version,
                                row.registration.configuration.portable,
                                inputs,
                                std::span<const std::byte>{feature.configuration}
                            );
                            if (!loaded)
                                return loaded;
                        }
                    }
                }
                else if (codec.valid())
                {
                    configuration_.control = {};
                    auto loaded = configuration_.create(
                        layout_,
                        source.configurationSchemaName(),
                        source.configurationSchemaVersion(),
                        codec,
                        inputs,
                        payload
                    );
                    if (!loaded)
                        return loaded;
                }
                if (!decoded)
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT,
                        "scene.configuration.decode",
                        0,
                        {},
                        decoded.error()
                    });
                if constexpr (requires { source.requirementBindingCount(); })
                    for (std::size_t i{}; i < source.requirementBindingCount(); ++i)
                    {
                        const auto binding = source.requirementBindingAt(i);
                        auto field = std::ranges::find_if(providers_, [&](const auto& item) {
                            return item->requirement == binding.requirement();
                        });
                        if (field == providers_.end())
                            return cxx::unexpected(SceneConfigurationFailure{
                                ESceneConfigurationError::MISSING_PROVIDER,
                                "scene.configuration.requirement"
                            });
                        auto& names = (*field)->names;
                        auto provider = std::ranges::find(names, binding.provider());
                        if (provider == names.end())
                            return cxx::unexpected(SceneConfigurationFailure{
                                ESceneConfigurationError::MISSING_PROVIDER,
                                "scene.configuration.provider"
                            });
                        (*field)->choice.setValue(provider - names.begin());
                    }
                return {};
            }
            void setStage(ESceneConfigurationStage stage) noexcept
            {
                const bool features = stage == ESceneConfigurationStage::FEATURES;
                const bool all = stage == ESceneConfigurationStage::ALL;
                name_.setVisible(!features);
                fields_.setVisible(!features);
                remove_.setVisible(!features);
                if (configuration_.control.content())
                    configuration_.control.content()->setVisible(!features);
                if (bootstrap_)
                    bootstrap_->setVisible(!features);
                for (auto& feature : features_)
                    feature->layout.setVisible(all || features);
                setVisible(
                    all || (simulation && stage == ESceneConfigurationStage::SIMULATION) ||
                    (scene && stage == ESceneConfigurationStage::SCENE) || (features && !features_.empty())
                );
            }
            void presetBindings() noexcept
            {
                for (auto& provider : providers_)
                    if (provider->names.size() == 2)
                        provider->choice.setValue(1);
            }
            SceneConfigurationResult<void> presetFeatures(const Inputs& inputs, ESceneContentPreset preset)
            {
                if (features_.empty())
                    return {};
                std::vector<std::string> wanted{"lux.render.view_camera.v1", "lux.render.material.v1"};
                if (preset == ESceneContentPreset::TWO_DIMENSIONAL)
                    wanted.emplace_back("lux.render.canvas2d.v2");
                else
                    for (auto name :
                         {"lux.render.mesh_stack.v1",
                          "lux.render.light.v1",
                          "lux.render.forward_mesh.v1",
                          "lux.render.shadow_map.v1"})
                        wanted.emplace_back(name);
                for (std::size_t i{}; i < wanted.size(); ++i)
                {
                    for (const auto& plugin : inputs.catalog.plugins())
                    {
                        for (const auto& feature : plugin.render_features)
                        {
                            if (feature.identity.id != wanted[i])
                                continue;
                            for (const auto& dependency : feature.dependencies)
                            {
                                const bool is_required_dependency = !dependency.optional;
                                const bool is_missing_dependency =
                                    is_required_dependency &&
                                    std::ranges::find(wanted, dependency.feature.id) == wanted.end();
                                if (is_missing_dependency)
                                    wanted.push_back(dependency.feature.id);
                            }
                        }
                    }
                }
                for (const auto& name : wanted)
                {
                    const auto found = std::ranges::find_if(features_, [&](const auto& feature) {
                        return feature->registration.factory.descriptor.canonical_name == name;
                    });
                    if (found == features_.end())
                        return lux::cxx::unexpected(SceneConfigurationFailure{
                            ESceneConfigurationError::MISSING_PROVIDER,
                            "scene.preset.feature",
                            0,
                            name
                        });
                    (*found)->enabled.setValue(true);
                }
                return {};
            }
            SystemId id;
            const lux::simulation::SimulationSystemRegistration* simulation;
            const lux::scene::SceneSystemRegistration* scene;
            bool remove{};

        private:
            controls::SizeHint sizeHintContent() noexcept override
            {
                return layout_.sizeHint();
            }
            controls::SizeHint measureContent(float width) noexcept override
            {
                return layout_.measure(width);
            }
            void arrangeContent() noexcept override
            {
                layout_.arrange({{}, rect().size});
            }
            void draw() noexcept override
            {
                drawChild(layout_);
            }
            controls::Layout layout_;
            controls::Label title_;
            controls::TextEdit name_;
            controls::Layout fields_;
            controls::Button remove_;
            std::vector<std::unique_ptr<NumberField>> numbers_;
            std::vector<std::unique_ptr<ProviderField>> providers_;
            std::vector<std::unique_ptr<FeatureField>> features_;
            std::unique_ptr<controls::CheckBox> bootstrap_;
            ConfigurationField configuration_;
            std::vector<lux::scene::RenderFeatureInstanceDescription> unknown_features_;
            object::Connection removed_;
        };

    }
    struct SceneConfigurationElement::Impl final
    {
        class Relations final : public controls::Element
        {
        public:
            Relations(controls::Element& parent, Impl& pane)
                : Element(parent, controls::ElementId{"relationships"}), owner_(pane)
            {
                setStretch({1, 0});
            }

        private:
            controls::SizeHint sizeHintContent() noexcept override
            {
                return {{300, 560}, {300, 560}};
            }
            void draw() noexcept override
            {
                owner_.drawRelations();
            }
            Impl& owner_;
        };

    public:
        Impl(SceneConfigurationElement& owner, Inputs inputs, SceneConfigurationResult<void>& status)
            : owner_(owner), inputs_(inputs), layout_(owner, controls::ElementId{"form"}),
              name_(layout_, controls::ElementId{"name"}, "Untitled scene"),
              note_(
                  layout_,
                  controls::ElementId{"note"},
                  "Single partition. Content presets do not change partition compatibility. Dependencies and "
                  "providers are saved explicitly."
              ),
              schemas_(layout_, controls::ElementId{"schemas"}, controls::ELayoutType::GRID),
              add_row_(layout_, controls::ElementId{"add"}, controls::ELayoutType::HORIZONTAL),
              system_type_(makeSystemChoice(add_row_, inputs, ESceneConfigurationStage::ALL)),
              add_(add_row_, controls::ElementId{"add-system"}, "Add system"),
              systems_(layout_, controls::ElementId{"systems"}), relations_(layout_, *this),
              error_(layout_, controls::ElementId{"error"})
        {
            note_.setWrap(true);
            schemas_.setColumns(2);
            layout_.setScrollable(false, true);
            for (const auto& schema : inputs.components.all())
            {
                const bool is_runtime_derived =
                    schema.semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED;
                const bool has_capture = static_cast<bool>(schema.capture);
                const bool is_capturable_schema = !is_runtime_derived && has_capture;
                if (is_capturable_schema)
                    schema_fields_.emplace_back(
                        lux::world::worldDataSchemaId(schema.id.name),
                        std::make_unique<controls::CheckBox>(
                            schemas_,
                            controls::ElementId{schema.id.name},
                            schema.id.name
                        )
                    );
            }
            connection_ = takeConnection(
                object::LuxObject::connect(
                    &add_,
                    &controls::Button::activated,
                    [this]() noexcept { add_requested_ = true; }
                ),
                status
            );
        }
        static std::vector<controls::ChoiceOption> systemOptions(const Inputs& inputs, ESceneConfigurationStage stage)
        {
            std::vector<controls::ChoiceOption> result;
            const auto simulation_types = inputs.catalog.systemsForWorld(
                lux::project::EMetadataSystemDomain::SIMULATION,
                "lux.spatial.builtin.single"
            );
            const auto scene_types = inputs.catalog.systemsForWorld(
                lux::project::EMetadataSystemDomain::SCENE,
                "lux.spatial.builtin.single"
            );
            const auto contains = [](auto types, std::string_view name) {
                return std::ranges::any_of(types, [name](const auto* type) { return type->identity.id == name; });
            };
            std::int64_t index{};
            for (const auto& system : inputs.simulation_systems.all())
            {
                const bool allows_simulation = stage != ESceneConfigurationStage::SCENE;
                const bool is_available_simulation = allows_simulation && contains(simulation_types, system.type.name);
                if (is_available_simulation)
                    result.push_back({index, "Simulation: " + system.type.name + origin(inputs, system.type.name)});
                ++index;
            }
            for (const auto& system : inputs.scene_systems)
            {
                const bool allows_scene = stage != ESceneConfigurationStage::SIMULATION;
                const bool is_available_scene = allows_scene && contains(scene_types, system.type.name);
                if (is_available_scene)
                    result.push_back({index, "Scene: " + system.type.name + origin(inputs, system.type.name)});
                ++index;
            }
            return result;
        }
        static std::unique_ptr<controls::Choice> makeSystemChoice(
            controls::Element& parent,
            const Inputs& inputs,
            ESceneConfigurationStage stage
        )
        {
            auto options = systemOptions(inputs, stage);
            const auto selected = options.empty() ? -1 : options.front().value;
            return std::make_unique<controls::Choice>(
                parent,
                controls::ElementId{"system-type"},
                std::move(options),
                selected
            );
        }
        template <class Failure>
        SceneConfigurationResult<SceneCreationConfiguration> rejected(std::string operation, Failure error)
        {
            return lux::cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::INVALID_ARGUMENT,
                std::move(operation),
                0,
                {},
                error
            });
        }
        SceneConfigurationResult<SceneCreationConfiguration> build()
        {
            finishControls(owner_);
            std::vector<lux::world::WorldDataSchemaId> schemas;
            for (const auto& [id, field] : schema_fields_)
                if (field->value())
                    schemas.push_back(id);
            lux::simulation::SimulationDescriptionBuilder simulation;
            lux::scene::SceneDescriptionBuilder scene;
            if (base_)
            {
                scene.setWorld(base_->world->id());
                scene.setSimulation(base_->simulation->id());
                const auto& source = base_->simulation->data();
                for (std::size_t i{}; i < source.dataCount(); ++i)
                {
                    const auto data = source.dataAt(i);
                    auto added = simulation.addData(data.schema(), data.version(), data.payload());
                    if (!added)
                        return rejected("scene.configuration.data", added.error());
                }
            }
            for (const auto& row : rows_)
            {
                auto encoded = row->encode();
                if (!encoded)
                    return lux::cxx::unexpected(encoded.error());
                if (row->simulation)
                {
                    auto added = simulation.addSystem(row->id, row->name(), *row->simulation->description, *encoded);
                    if (!added)
                        return rejected("scene.new.simulation", added.error());
                }
                else
                {
                    const auto& type = row->description();
                    auto added = scene.addSystem(
                        row->id,
                        row->name(),
                        row->scene->type,
                        type.version,
                        type.configuration_schema_name,
                        type.configuration_schema_version,
                        *encoded
                    );
                    if (!added)
                        return rejected("scene.new.system", added.error());
                    auto bound = row->bind(scene);
                    if (!bound)
                        return lux::cxx::unexpected(bound.error());
                }
            }
            for (const auto& [before, after] : construction_)
            {
                auto added = simulation.addConstructionDependency(before, after);
                if (!added)
                    return rejected("scene.new.construction", added.error());
            }
            for (const auto& [before, after] : scene_dependencies_)
            {
                auto added = scene.addDependency(before, after);
                if (!added)
                    return rejected("scene.new.scene-dependency", added.error());
            }
            for (const auto& edge : execution_)
            {
                auto added = simulation.addExecutionDependency(edge.before, edge.after);
                if (!added)
                    return rejected("scene.new.execution", added.error());
            }
            for (const auto& producer : producers_)
            {
                auto added = simulation.addChannelProducer(producer);
                if (!added)
                    return rejected("scene.new.channel", added.error());
            }
            auto sim = std::move(simulation).build();
            if (!sim)
                return rejected("scene.new.simulation", sim.error());
            auto desc = std::move(scene).buildResolved();
            if (!desc)
                return rejected("scene.new.scene", desc.error());
            return SceneCreationConfiguration{
                name_.value(),
                std::move(schemas),
                std::make_shared<const lux::simulation::SimulationDescription>(std::move(*sim)),
                std::move(*desc),
                viewport_
            };
        }
        SceneConfigurationResult<void> load(const SceneConfiguration& value, SystemId viewport)
        {
            // This is an off-tree candidate. Unknown payloads are preserved, unsupported schema changes refused.
            if (!value.scene || !value.world || !value.simulation)
                return cxx::unexpected(
                    SceneConfigurationFailure{ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.input"}
                );
            base_ = value;
            for (auto& [id, field] : schema_fields_)
                field->setValue(
                    std::ranges::find(value.world->data().schemas(), id) != value.world->data().schemas().end()
                );
            const auto& name = value.scene->info().display_name;
            name_.setValue(std::string(name.begin(), std::ranges::find(name, '\0')));
            rows_.clear();
            construction_.clear();
            scene_dependencies_.clear();
            execution_.clear();
            producers_.clear();
            const auto& simulation = value.simulation->data();
            const auto& scene = value.scene->data();
            SceneConfigurationResult<void> status;
            next_system_ = 1;
            const auto add = [&](auto source, auto* simulation_registration, auto* scene_registration
                             ) -> SceneConfigurationResult<void> {
                auto row = std::make_unique<SystemElement>(
                    systems_,
                    source.instanceId(),
                    inputs_,
                    simulation_registration,
                    scene_registration,
                    status
                );
                if (!status)
                    return status;
                auto loaded = row->load(source, inputs_);
                if (!loaded)
                    return loaded;
                if (source.instanceId().value == UINT64_MAX)
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT,
                        "scene.configuration.identity.exhausted"
                    });
                next_system_ = std::max(next_system_, source.instanceId().value + 1);
                rows_.push_back(std::move(row));
                return {};
            };
            for (std::size_t i{}; i < simulation.systemCount(); ++i)
            {
                const auto source = simulation.systemAt(i);
                const auto found = std::ranges::find(
                    inputs_.simulation_systems.all(),
                    source.type(),
                    &lux::simulation::SimulationSystemRegistration::type
                );
                if (found == inputs_.simulation_systems.all().end())
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::MISSING_PROVIDER,
                        "scene.configuration.simulation",
                        0,
                        source.type().name
                    });
                status = add(source, &*found, static_cast<const lux::scene::SceneSystemRegistration*>(nullptr));
                if (!status)
                    return status;
            }
            for (std::size_t i{}; i < scene.systemCount(); ++i)
            {
                const auto source = scene.systemAt(i);
                const auto found =
                    std::ranges::find(inputs_.scene_systems, source.type(), &lux::scene::SceneSystemRegistration::type);
                if (found == inputs_.scene_systems.end())
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::MISSING_PROVIDER,
                        "scene.configuration.system",
                        0,
                        source.type().name
                    });
                status =
                    add(source, static_cast<const lux::simulation::SimulationSystemRegistration*>(nullptr), &*found);
                if (!status)
                    return status;
            }
            for (std::size_t i{}; i < simulation.constructionDependencyCount(); ++i)
            {
                const auto edge = simulation.constructionDependencyAt(i);
                construction_.emplace_back(edge.before().instanceId(), edge.after().instanceId());
            }
            for (std::size_t i{}; i < scene.dependencyCount(); ++i)
            {
                const auto edge = scene.dependencyAt(i);
                scene_dependencies_.emplace_back(edge.before(), edge.after());
            }
            execution_.assign(simulation.executionDependencies().begin(), simulation.executionDependencies().end());
            producers_.assign(simulation.channelProducers().begin(), simulation.channelProducers().end());
            viewport_ = viewport;
            // Partition/schema edits require the model's separate structural operations.
            schemas_.setEnabled(false);
            name_.setEnabled(false);
            rebuildEndpoints();
            return {};
        }
        void update() noexcept
        {
            bool changed{};
            if (std::exchange(add_requested_, false))
            {
                const auto simulations = inputs_.simulation_systems.all();
                const auto scenes = std::span(inputs_.scene_systems);
                const auto index = static_cast<std::size_t>(system_type_->value());
                if (index < simulations.size() + scenes.size())
                {
                    SceneConfigurationResult<void> status;
                    auto row = std::make_unique<SystemElement>(
                        systems_,
                        SystemId{next_system_++},
                        inputs_,
                        index < simulations.size() ? &simulations[index] : nullptr,
                        index >= simulations.size() ? &scenes[index - simulations.size()] : nullptr,
                        status
                    );
                    if (status)
                    {
                        rows_.push_back(std::move(row));
                        changed = true;
                    }
                    else
                        error_.setText(status.error().domain + ": " + status.error().message);
                }
            }
            for (auto it = rows_.begin(); it != rows_.end();)
                if ((*it)->remove)
                {
                    finishControls(**it);
                    it = rows_.erase(it);
                    changed = true;
                }
                else
                    ++it;
            if (changed)
                rebuildEndpoints();
            setStage(stage_);
        }
        struct Endpoint final
        {
            std::string label;
            lux::simulation::SimulationExecutionPoint point;
        };
        struct Channel final
        {
            std::string label;
            SystemId system;
            lux::simulation::EventPointId event;
        };
        void rebuildEndpoints()
        {
            points_.clear();
            channels_.clear();
            for (const auto& row : rows_)
            {
                if (!row->simulation)
                    continue;
                const auto& type = *row->simulation->description;
                for (const auto& task : type.tasks)
                    points_.push_back(
                        {row->name() + "/" + std::string(task.name),
                         lux::simulation::SimulationExecutionPoint::task(row->id, task.id)}
                    );
                for (const auto& hook : type.hooks)
                    points_.push_back(
                        {row->name() + "/hook/" + std::string(hook.diagnostic_name),
                         lux::simulation::SimulationExecutionPoint::hook(row->id, hook.id)}
                    );
                for (const auto& event : type.events)
                    channels_.push_back({row->name() + "/" + std::string(event.diagnostic_name), row->id, event.id});
            }
            point_before_ = point_after_ = channel_ = producer_ = 0;
        }
        template <class Items> void itemCombo(const char* label, std::size_t& selected, const Items& items)
        {
            if (ImGui::BeginCombo(label, selected < items.size() ? items[selected].label.c_str() : "Select"))
            {
                for (std::size_t i{}; i < items.size(); ++i)
                    if (ImGui::Selectable(items[i].label.c_str(), i == selected))
                        selected = i;
                ImGui::EndCombo();
            }
        }
        void systemCombo(const char* label, SystemId& selected, bool simulation, bool render_only = false)
        {
            const auto found = std::ranges::find(rows_, selected, [](const auto& row) { return row->id; });
            if (ImGui::BeginCombo(label, found != rows_.end() ? (*found)->name().c_str() : "Select"))
            {
                for (const auto& row : rows_)
                {
                    if (static_cast<bool>(row->simulation) != simulation)
                        continue;
                    const bool is_wrong_render_type =
                        render_only && row->scene->cpp_type != lux::cxx::typeToken<lux::scene::RenderSystem>();
                    if (is_wrong_render_type)
                        continue;
                    if (ImGui::Selectable(row->name().c_str(), row->id == selected))
                        selected = row->id;
                }
                ImGui::EndCombo();
            }
        }
        void drawRelations()
        {
            ImGui::TextUnformatted("Explicit relationships (not inferred from registration order)");
            systemCombo("Viewport RenderSystem", viewport_, false, true);
            systemCombo("Construct before", construct_before_, true);
            systemCombo("Construct after", construct_after_, true);
            const bool has_construction_pair = construct_before_.valid() && construct_after_.valid();
            const bool is_construction_requested = ImGui::Button("Add construction dependency");
            const bool should_add_construction = is_construction_requested && has_construction_pair;
            if (should_add_construction)
                construction_.emplace_back(construct_before_, construct_after_);
            systemCombo("Scene before", scene_before_, false);
            systemCombo("Scene after", scene_after_, false);
            const bool has_scene_pair = scene_before_.valid() && scene_after_.valid();
            const bool is_scene_requested = ImGui::Button("Add Scene dependency");
            const bool should_add_scene = is_scene_requested && has_scene_pair;
            if (should_add_scene)
                scene_dependencies_.emplace_back(scene_before_, scene_after_);
            itemCombo("Execute before", point_before_, points_);
            itemCombo("Execute after", point_after_, points_);
            const bool has_execution_source = point_before_ < points_.size();
            const bool has_execution_target = point_after_ < points_.size();
            const bool has_execution_pair = has_execution_source && has_execution_target;
            const bool is_execution_requested = ImGui::Button("Add execution dependency");
            const bool should_add_execution = is_execution_requested && has_execution_pair;
            if (should_add_execution)
                execution_.push_back({points_[point_before_].point, points_[point_after_].point});
            itemCombo("Channel", channel_, channels_);
            itemCombo("Producer task", producer_, points_);
            const bool has_channel = channel_ < channels_.size();
            const bool has_producer = producer_ < points_.size();
            const bool has_channel_producer = has_channel && has_producer;
            const bool is_producer_requested = ImGui::Button("Add channel producer");
            const bool should_add_producer = is_producer_requested && has_channel_producer;
            if (should_add_producer)
            {
                const auto point = points_[producer_].point;
                if (point.kind == lux::simulation::ESimulationExecutionPoint::SYSTEM_TASK)
                    producers_.push_back(
                        {channels_[channel_].system, channels_[channel_].event, point.system, {point.point}}
                    );
            }
            ImGui::Text(
                "Construction %zu, Scene %zu, execution %zu, producers %zu",
                construction_.size(),
                scene_dependencies_.size(),
                execution_.size(),
                producers_.size()
            );
            if (ImGui::Button("Clear relationships"))
            {
                construction_.clear();
                scene_dependencies_.clear();
                execution_.clear();
                producers_.clear();
            }
        }
        void setStage(ESceneConfigurationStage stage) noexcept
        {
            const bool has_stage_changed = stage_ != stage;
            const bool is_system_stage = stage == ESceneConfigurationStage::ALL ||
                                         stage == ESceneConfigurationStage::SIMULATION ||
                                         stage == ESceneConfigurationStage::SCENE;
            const bool needs_system_choice = has_stage_changed && is_system_stage;
            if (needs_system_choice)
            {
                system_type_.reset();
                system_type_ = makeSystemChoice(add_row_, inputs_, stage);
            }
            stage_ = stage;
            add_.setEnabled(system_type_->value() >= 0);
            const bool all = stage == ESceneConfigurationStage::ALL;
            name_.setVisible(all || stage == ESceneConfigurationStage::CONTENT);
            schemas_.setVisible(all || stage == ESceneConfigurationStage::CONTENT);
            add_row_.setVisible(
                all || stage == ESceneConfigurationStage::SIMULATION || stage == ESceneConfigurationStage::SCENE
            );
            relations_.setVisible(all || stage == ESceneConfigurationStage::RELATIONSHIPS);
            for (auto& row : rows_)
                row->setStage(stage);
        }
        SceneConfigurationResult<void> applyPreset(ESceneContentPreset preset)
        {
            finishControls(owner_);
            base_.reset();
            rows_.clear();
            construction_.clear();
            scene_dependencies_.clear();
            execution_.clear();
            producers_.clear();
            viewport_ = {};
            next_system_ = 1;
            for (auto& [id, field] : schema_fields_)
                field->setValue(
                    preset != ESceneContentPreset::EMPTY &&
                    (id.name == "lux.ecs.Parent" || id.name == "lux.scene.Camera" ||
                     (preset == ESceneContentPreset::TWO_DIMENSIONAL
                          ? id.name == "lux.ecs.Transform2D"
                          : (id.name == "lux.ecs.Transform3D" || id.name == "lux.ecs.Mesh3D" ||
                             id.name == "lux.ecs.Light3D")))
                );
            if (preset == ESceneContentPreset::EMPTY)
            {
                rebuildEndpoints();
                return {};
            }
            SceneConfigurationResult<void> status;
            for (const auto name : {"lux.scene.transform", "lux.scene.world_loading", "lux.builtin.system.render"})
            {
                const auto found = std::ranges::find(inputs_.scene_systems, std::string{name}, [](const auto& value) {
                    return value.type.name;
                });
                if (found == inputs_.scene_systems.end())
                    return lux::cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::MISSING_PROVIDER,
                        "scene.preset.system",
                        0,
                        name
                    });
                auto row = std::make_unique<SystemElement>(
                    systems_,
                    SystemId{next_system_++},
                    inputs_,
                    nullptr,
                    &*found,
                    status
                );
                if (!status)
                    return status;
                row->presetBindings();
                if (found->cpp_type == lux::cxx::typeToken<lux::scene::RenderSystem>())
                {
                    status = row->presetFeatures(inputs_, preset);
                    if (!status)
                        return status;
                    viewport_ = row->id;
                }
                rows_.push_back(std::move(row));
            }
            rebuildEndpoints();
            setStage(stage_);
            return {};
        }
        std::optional<SceneConfiguration> base_;
        SceneConfigurationElement& owner_;
        Inputs inputs_;
        controls::Layout layout_;
        controls::TextEdit name_;
        controls::Label note_;
        controls::Layout schemas_, add_row_;
        std::unique_ptr<controls::Choice> system_type_;
        controls::Button add_;
        controls::Layout systems_;
        Relations relations_;
        controls::Label error_;
        std::vector<std::pair<lux::world::WorldDataSchemaId, std::unique_ptr<controls::CheckBox>>> schema_fields_;
        std::vector<std::unique_ptr<SystemElement>> rows_;
        std::vector<std::pair<SystemId, SystemId>> construction_, scene_dependencies_;
        std::vector<lux::simulation::SimulationExecutionDependency> execution_;
        std::vector<lux::simulation::SimulationChannelProducer> producers_;
        std::vector<Endpoint> points_;
        std::vector<Channel> channels_;
        SystemId viewport_, construct_before_, construct_after_, scene_before_, scene_after_;
        std::size_t point_before_{}, point_after_{}, channel_{}, producer_{};
        std::uint64_t next_system_{1};
        bool add_requested_{};
        object::Connection connection_;
        ESceneConfigurationStage stage_{ESceneConfigurationStage::ALL};
    };

    SceneConfigurationElement::SceneConfigurationElement(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        SceneConfigurationInputs inputs,
        SceneConfigurationResult<void>& status
    )
        : Element(parent, id), impl_(std::make_unique<Impl>(*this, std::move(inputs), status))
    {
        setStretch({1, 1});
    }
    SceneConfigurationElement::~SceneConfigurationElement() noexcept = default;
    SceneConfigurationResult<SceneCreationConfiguration> SceneConfigurationElement::build()
    {
        return impl_->build();
    }
    SceneConfigurationResult<void> SceneConfigurationElement::applyPreset(ESceneContentPreset preset)
    {
        return impl_->applyPreset(preset);
    }
    SceneConfigurationResult<void> SceneConfigurationElement::load(const SceneConfiguration& value, SystemId viewport)
    {
        return impl_->load(value, viewport);
    }
    SceneConfigurationResult<SceneSetConfiguration> SceneConfigurationElement::buildEdit(const SceneConfiguration& base)
    {
        if (!base.scene || !base.world || !base.simulation)
            return cxx::unexpected(
                SceneConfigurationFailure{ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.input"}
            );
        auto built = impl_->build();
        if (!built)
            return cxx::unexpected(built.error());
        const auto copied = [](auto values) {
            return std::vector<asset::AssetAuxiliaryPayload>(values.begin(), values.end());
        };
        auto simulation = simulation::SimulationAsset::create(
            base.simulation->info(),
            std::move(built->simulation),
            copied(base.simulation->auxiliaryPayloads())
        );
        if (!simulation)
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::INVALID_ARGUMENT,
                "scene.configuration.simulation.asset",
                0,
                {},
                simulation.error()
            });
        auto scene = lux::scene::SceneAsset::create(
            base.scene->info(),
            std::make_shared<const lux::scene::SceneDescription>(std::move(built->scene)),
            copied(base.scene->auxiliaryPayloads())
        );
        if (!scene)
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::INVALID_ARGUMENT,
                "scene.configuration.scene.asset",
                0,
                {},
                scene.error()
            });
        return SceneSetConfiguration{{std::move(*scene), base.world, std::move(*simulation)}};
    }
    void SceneConfigurationElement::setStage(ESceneConfigurationStage stage) noexcept
    {
        impl_->setStage(stage);
    }
    lux::ui::SizeHint SceneConfigurationElement::sizeHintContent() noexcept
    {
        return impl_->layout_.sizeHint();
    }
    lux::ui::SizeHint SceneConfigurationElement::measureContent(float width) noexcept
    {
        return impl_->layout_.measure(width);
    }
    void SceneConfigurationElement::arrangeContent() noexcept
    {
        impl_->layout_.arrange({{}, rect().size});
    }
    void SceneConfigurationElement::draw() noexcept
    {
        drawChild(impl_->layout_);
    }
    void SceneConfigurationElement::update() noexcept
    {
        impl_->update();
    }
}
