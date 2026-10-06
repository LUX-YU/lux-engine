#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/scene/AuthoringFacts.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/TransformSystem.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
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
        }
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
            controls::Label applicability;
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
                  ), applicability(layout, controls::ElementId{"applicability"})
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
                    // Loading a form must not reorder the source's feature payloads. New selections
                    // follow the existing rows; absent plugin controls keep their original position.
                    const auto rank = [&](const auto& feature) {
                        return std::ranges::find(feature_order_, feature.type) - feature_order_.begin();
                    };
                    std::ranges::stable_sort(value.features, {}, rank);
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
            SceneConfigurationResult<SceneSystemConfigurationDraft> capture() const
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
            SceneConfigurationResult<void> load(const SceneSystemConfigurationDraft& source, const Inputs& inputs)
            {
                const auto& expected = description();
                const bool is_version_mismatch = source.version != expected.version;
                const bool is_schema_mismatch =
                    source.configuration_schema != expected.configuration_schema_name ||
                    source.configuration_version != expected.configuration_schema_version;
                if (is_version_mismatch || is_schema_mismatch)
                    return cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::INVALID_ARGUMENT,
                        "scene.configuration.version"
                    });
                name_.setValue(std::string(source.name));
                const auto& codec = simulation ? simulation->configuration : scene->configuration;
                const std::span<const std::byte> payload = source.configuration;
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
                            feature_order_.push_back(feature.type);
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
                            {
                                row.enabled.setEnabled(false);
                                unknown_features_.push_back(feature);
                                continue;
                            }
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
                        source.configuration_schema,
                        source.configuration_version,
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
                for (const auto& binding : source.providers)
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
            SceneConfigurationResult<void> checkFeatures(
                const AuthoringFacts& facts, lux::scene::RenderFeatureSceneBindings bindings
            )
            {
                SceneConfigurationResult<void> result;
                for (const auto& feature : features_)
                {
                    const auto type = feature->registration.factory.descriptor.type;
                    const auto binding = std::ranges::find(bindings, type, &lux::scene::RenderFeatureSceneBinding::feature);
                    const auto required = binding == bindings.end() ? std::span<const std::string_view>{}
                                                                   : binding->author_inputs;
                    const auto allowed = queryApplicability(facts, {required});
                    // An existing selection stays visible and may be explicitly removed. Never drop
                    // unknown configurations or silently change the user's selected feature set.
                    feature->enabled.setEnabled(allowed.supported() || feature->enabled.value());
                    feature->applicability.setText(allowed.supported() ? "" : "Missing author input: " + allowed.subject);
                    if (!allowed.supported() && feature->enabled.value() && result)
                        result = cxx::unexpected(SceneConfigurationFailure{
                            ESceneConfigurationError::INVALID_ARGUMENT, "scene.feature.author-input",
                            static_cast<std::uint64_t>(allowed.reason), allowed.subject
                        });
                }
                return result;
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
            std::vector<render::FeatureTypeId> feature_order_;
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
                if (isAuthorComponent(schema))
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
        static std::vector<controls::ChoiceOption> systemOptions(const Inputs& inputs, ESceneConfigurationStage stage, std::string_view partition)
        {
            std::vector<controls::ChoiceOption> result;
            const auto simulation_types = inputs.catalog.systemsForWorld(
                lux::project::EMetadataSystemDomain::SIMULATION,
                partition
            );
            const auto scene_types = inputs.catalog.systemsForWorld(
                lux::project::EMetadataSystemDomain::SCENE,
                partition
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
            ESceneConfigurationStage stage, std::string_view partition = "lux.spatial.builtin.single"
        )
        {
            auto options = systemOptions(inputs, stage, partition);
            const auto selected = options.empty() ? -1 : options.front().value;
            return std::make_unique<controls::Choice>(
                parent,
                controls::ElementId{"system-type"},
                std::move(options),
                selected
            );
        }
        AuthoringFacts facts()
        {
            if (origin_.base)
                return authoringFacts(origin_.base->world->data(), inputs_.components, origin_.based_on);
            std::size_t position{};
            bool changed{};
            for (const auto& [id, field] : schema_fields_)
                if (field->value())
                {
                    changed |= position >= selected_schemas_.size() || selected_schemas_[position] != id;
                    ++position;
                }
            changed |= position != selected_schemas_.size();
            if (changed)
            {
                selected_schemas_.clear();
                for (const auto& [id, field] : schema_fields_)
                    if (field->value())
                        selected_schemas_.push_back(id);
            }
            return {origin_.based_on, selected_schemas_, inputs_.components, origin_.partition, origin_.partition_version};
        }
        SceneConfigurationResult<SceneConfigurationDraft> capture()
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
            const auto context = facts();
            for (const auto& row : rows_)
                if (auto supported = row->checkFeatures(context, inputs_.feature_bindings); !supported)
                    error_.setText(supported.error().domain + ": " + supported.error().message);
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
                system_type_ = makeSystemChoice(add_row_, inputs_, stage, origin_.partition);
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
            auto draft = makeSceneConfigurationPreset(preset, "lux.spatial.builtin.single", 1, inputs_.registrations());
            if (!draft)
                return cxx::unexpected(presentationFailure(std::move(draft.error())));
            return loadDraft(std::move(*draft));
        }
        SceneConfigurationDraft origin_{{}, {}, "Untitled scene", "lux.spatial.builtin.single", 1};
        std::vector<SceneSystemConfigurationDraft> opaque_systems_;
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
        std::vector<world::WorldDataSchemaId> selected_schemas_;
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
    SceneConfigurationResult<SceneConfigurationDraft> SceneConfigurationElement::capture()
    {
        return impl_->capture();
    }
    SceneConfigurationResult<SceneCreationConfiguration> SceneConfigurationElement::build()
    {
        return impl_->build();
    }
    SceneConfigurationResult<void> SceneConfigurationElement::applyPreset(ESceneContentPreset preset)
    {
        return impl_->applyPreset(preset);
    }
    SceneConfigurationResult<void> SceneConfigurationElement::load(
        const SceneConfiguration& value, SystemId viewport, sessions::ContentStamp based_on
    )
    {
        return impl_->load(value, viewport, based_on);
    }
    SceneConfigurationResult<SceneSetConfiguration> SceneConfigurationElement::buildEdit(const SceneConfiguration& base)
    {
        auto draft = impl_->capture();
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
