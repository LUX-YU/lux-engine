#pragma once

#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/editor/scene/ConfigurationEditor.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
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
            std::any>;
        ESceneConfigurationError code{};
        std::string domain;
        std::uint64_t reason{};
        std::string message;
        VCause cause;
    };
    template <class T> using SceneConfigurationResult = cxx::expected<T, SceneConfigurationFailure>;

    struct SceneProviderOption final
    {
        std::string_view capability, name;
    };
    struct SceneCreationConfiguration final
    {
        std::string name;
        std::vector<world::WorldDataSchemaId> schemas;
        std::shared_ptr<const simulation::SimulationDescription> simulation;
        lux::scene::SceneDescription scene;
        system::SystemInstanceId viewport;
    };
    enum class ESceneConfigurationStage : std::uint8_t
    {
        ALL,
        CONTENT,
        SIMULATION,
        SCENE,
        FEATURES,
        RELATIONSHIPS
    };
    enum class ESceneContentPreset : std::uint8_t
    {
        EMPTY,
        TWO_DIMENSIONAL,
        THREE_DIMENSIONAL
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
        const lux::project::PluginCatalog& catalog;
        const simulation::ecs::ComponentSchemaSet& components;
        const simulation::SimulationSystemRegistry& simulation_systems;
        std::span<const lux::scene::SceneSystemRegistration> scene_systems;
        std::span<const render::RenderFeatureRegistration> features;
        std::span<const SceneProviderOption> providers;
        // An empty control uses the registered codec's default. Factories return complete owning controls.
        std::function<SceneConfigurationResult<
            ConfigurationControl>(lux::ui::Element&, std::string_view, std::uint32_t, const serialization::PortableValueCodec&, std::optional<std::span<const std::byte>>)>
            configuration;
    };
    class SceneConfigurationElement final : public lux::ui::Element
    {
    public:
        SceneConfigurationElement(lux::ui::Element&, lux::ui::ElementId, SceneConfigurationInputs, SceneConfigurationResult<void>&);
        ~SceneConfigurationElement() noexcept override;
        SceneConfigurationElement(const SceneConfigurationElement&) = delete;
        SceneConfigurationElement& operator=(const SceneConfigurationElement&) = delete;
        SceneConfigurationElement(SceneConfigurationElement&&) = delete;
        SceneConfigurationElement& operator=(SceneConfigurationElement&&) = delete;
        [[nodiscard]] SceneConfigurationResult<SceneCreationConfiguration> build();
        [[nodiscard]] SceneConfigurationResult<void> applyPreset(ESceneContentPreset);
        // Load only into a candidate form; failed loads do not alter any Session or the mounted form.
        [[nodiscard]] SceneConfigurationResult<void> load(
            const SceneConfiguration&,
            system::SystemInstanceId viewport = {}
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
