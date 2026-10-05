#pragma once

#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/editor/scene/SceneConfigurationPreparation.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
#include <lux/engine/function/render/client/core/RenderFeatureRegistration.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/world/WorldDataSchemaId.hpp>
#include <functional>
#include <variant>
#include <any>
#include <lux/engine/editor/scene/SceneEdit.hpp>

namespace lux::services { class ServiceEntry; }

namespace lux::scene { struct RenderFeatureSceneBinding; }

namespace lux::editor::scene
{
    enum class ESceneConfigurationError : std::uint8_t
    {
        INVALID_ARGUMENT,
        MISSING_PROVIDER,
        CONTROL_FAILURE,
        BUSY
    };
    struct SceneConfigurationFailure final
    {
        using VCause = std::variant<
            std::monostate,
            serialization::SerializationFailure,
            simulation::SimulationDescriptionFailure,
            lux::scene::SceneDescriptionFailure,
            object::EConnectError,
            asset::AssetDecodeFailure,
            SceneEditError,
            std::any,
            ScenePreparationFailure
        >;
        ESceneConfigurationError code{};
        std::string domain;
        std::uint64_t reason{};
        std::string message;
        VCause cause;
    };
    template <class T> using SceneConfigurationResult = cxx::expected<T, SceneConfigurationFailure>;

    enum class ESceneConfigurationStage : std::uint8_t
    {
        ALL,
        CONTENT,
        SIMULATION,
        SCENE,
        FEATURES,
        RELATIONSHIPS
    };
    // Owns only the configuration form's draft value and its code, never author/session state.
    // Assignment first destroys the old control, then its draft/code. No last-lease destructor race.
    class ConfigurationControl final
    {
    public:
        using Encode = serialization::SerializationResult (*)(const void*, std::vector<std::byte>&) noexcept;
        ConfigurationControl() = default;
        ConfigurationControl(
            std::shared_ptr<const void> value,
            std::unique_ptr<lux::ui::Element> content,
            Encode encode
        )
            : value_(std::move(value)), content_(std::move(content)), encode_(encode)
        {}
        ConfigurationControl(ConfigurationControl&&) noexcept = default;
        ConfigurationControl& operator=(ConfigurationControl&& other) noexcept
        {
            if (this != &other)
            {
                ConfigurationControl old(std::move(*this));
                swap(other);
            }
            return *this;
        }
        ConfigurationControl(const ConfigurationControl&) = delete;
        ConfigurationControl& operator=(const ConfigurationControl&) = delete;
        [[nodiscard]] explicit operator bool() const noexcept
        {
            return value_ && content_ && encode_;
        }
        [[nodiscard]] lux::ui::Element* content() const noexcept
        {
            return content_.get();
        }
        [[nodiscard]] serialization::SerializationResult encode(std::vector<std::byte>& bytes) const noexcept
        {
            return encode_(value_.get(), bytes);
        }

    private:
        void swap(ConfigurationControl& other) noexcept
        {
            value_.swap(other.value_);
            content_.swap(other.content_);
            std::swap(encode_, other.encode_);
        }
        std::shared_ptr<const void> value_;
        std::unique_ptr<lux::ui::Element> content_;
        Encode encode_{};
    };
    [[nodiscard]] SceneConfigurationResult<ConfigurationControl> makeConfigurationControl(
        const ConfigurationEditor&,
        lux::ui::Element&,
        lux::ui::ElementId,
        std::optional<std::span<const std::byte>> initial = {}
    );

    struct SceneConfigurationInputs final
    {
        using CreateControlResult = SceneConfigurationResult<ConfigurationControl>;
        using CreateControl = std::function<CreateControlResult(
            lux::ui::Element&,
            std::string_view,
            std::uint32_t,
            const serialization::PortableValueCodec&,
            std::optional<std::span<const std::byte>>
        )>;
        const lux::project::PluginCatalog& catalog;
        const simulation::ecs::ComponentSchemaSet& components;
        const simulation::SimulationSystemRegistry& simulation_systems;
        std::span<const lux::scene::SceneSystemRegistration> scene_systems;
        std::span<const render::RenderFeatureRegistration> features;
        std::span<const SceneProviderOption> providers;
        // An empty control uses the registered codec's default. Factories return complete owning controls.
        CreateControl configuration;
        std::span<const lux::scene::RenderFeatureSceneBinding> feature_bindings{};
        [[nodiscard]] SceneConfigurationRegistrations registrations() const noexcept
        {
            return {components, simulation_systems, scene_systems, features, providers, feature_bindings};
        }
    };
    // Freeze only the selected editor definitions. Foundation borrows remain explicit; a temporary
    // project-creation environment supplies its owning lifetime here. No live catalog is retained.
    [[nodiscard]] SceneConfigurationResult<SceneConfigurationInputs> makeSceneConfigurationInputs(
        const lux::project::PluginCatalog&,
        const SceneConfigurationRegistrations&,
        std::span<const std::shared_ptr<const services::ServiceEntry>>,
        std::shared_ptr<const void> foundation_owner = {}
    );
    [[nodiscard]] std::span<const SceneProviderOption> defaultSceneProviders() noexcept;

    class SceneConfigurationElement final : public lux::ui::Element
    {
    public:
        SceneConfigurationElement(
            lux::ui::Element&,
            lux::ui::ElementId,
            SceneConfigurationInputs,
            SceneConfigurationResult<void>&
        );
        ~SceneConfigurationElement() noexcept override;
        SceneConfigurationElement(const SceneConfigurationElement&) = delete;
        SceneConfigurationElement& operator=(const SceneConfigurationElement&) = delete;
        SceneConfigurationElement(SceneConfigurationElement&&) = delete;
        SceneConfigurationElement& operator=(SceneConfigurationElement&&) = delete;
        [[nodiscard]] SceneConfigurationResult<SceneConfigurationDraft> capture();
        [[nodiscard]] SceneConfigurationResult<SceneCreationConfiguration> build();
        [[nodiscard]] SceneConfigurationResult<void> applyPreset(ESceneContentPreset);
        // Load only into a candidate form; failed loads do not alter any Session or the mounted form.
        [[nodiscard]] SceneConfigurationResult<void> load(
            const SceneConfiguration&,
            system::SystemInstanceId viewport = {},
            sessions::ContentStamp based_on = {}
        );
        [[nodiscard]] SceneConfigurationResult<SceneSetConfiguration> buildEdit(const SceneConfiguration&);
        void setStage(ESceneConfigurationStage) noexcept;

    private:
        lux::ui::SizeHint sizeHintContent() noexcept override;
        lux::ui::SizeHint measureContent(float) noexcept override;
        void arrangeContent() noexcept override;
        void draw() noexcept override;
        void update() noexcept override;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
