#include <lux/engine/editor/ui/SceneConfigurationElement.hpp>
#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/configuration/ConfigurationValue.hpp>
#include <algorithm>

namespace lux::editor::ui
{
    namespace
    {
        EditorFailure translate(const scene::SceneConfigurationFailure& error)
        {
            const auto code = error.code == scene::ESceneConfigurationError::MISSING_PROVIDER
                                  ? EEditorError::MISSING_PROVIDER
                                  : EEditorError::INVALID_ARGUMENT;
            return {code, error.domain, error.reason, error.message, error.cause};
        }
        template <class Failure> auto rejected(Failure error)
        {
            return cxx::unexpected(scene::SceneConfigurationFailure{
                scene::ESceneConfigurationError::CONTROL_FAILURE,
                "scene.configuration.legacy",
                0,
                {},
                std::any{std::move(error)}
            });
        }
    }
    // P12 bridge, used only by the old launcher and its retained configuration tests.
    // The entire form/preset/build algorithm lives in scene_ui.
    struct SceneConfigurationElement::Impl final
    {
        std::vector<scene::SceneProviderOption> providers_;
        std::unique_ptr<scene::SceneConfigurationElement> form_;
        Impl(
            SceneConfigurationElement& owner,
            const lux::project::PluginCatalog& catalog,
            const lux::project::SceneRegistrations& registrations,
            std::span<const ConfigurationEditorRegistration> configurations,
            std::span<const SceneProviderOption> providers,
            EditorResult<void>& status
        )
        {
            for (const auto& value : providers)
                providers_.push_back({value.capability, value.name});
            scene::SceneConfigurationInputs inputs{
                catalog,
                registrations.components,
                *registrations.simulation_systems,
                registrations.scene_systems,
                registrations.features,
                providers_
            };
            inputs.configuration = [configurations](
                                       lux::ui::Element& parent,
                                       std::string_view schema,
                                       std::uint32_t version,
                                       const serialization::PortableValueCodec& codec,
                                       std::optional<std::span<const std::byte>> initial
                                   ) -> scene::SceneConfigurationResult<scene::ConfigurationControl> {
                const auto found = std::ranges::find_if(configurations, [&](const auto& item) {
                    return schema == item.schema_name && version == item.schema_version &&
                           codec.type == item.codec.type;
                });
                if (found == configurations.end())
                    return scene::ConfigurationControl{};
                auto value = ConfigurationValue::create(
                    {found->schema_name, found->schema_version, found->codec, found->reflection},
                    found->code_lifetime
                );
                if (!value)
                    return rejected(value.error());
                auto owner = std::make_shared<ConfigurationValue>(std::move(*value));
                if (initial)
                {
                    const auto decoded = owner->decode(*initial);
                    if (!decoded)
                        return rejected(decoded.error());
                }
                auto content = found->create(parent, lux::ui::ElementId{"configuration"}, *owner);
                if (!content)
                    return rejected(content.error());
                return scene::ConfigurationControl{
                    std::move(owner),
                    std::move(*content),
                    [](const void* value, std::vector<std::byte>& bytes) noexcept {
                        return static_cast<const ConfigurationValue*>(value)->encode(bytes);
                    }
                };
            };
            scene::SceneConfigurationResult<void> created;
            form_ = std::make_unique<scene::SceneConfigurationElement>(
                owner,
                lux::ui::ElementId{"form"},
                std::move(inputs),
                created
            );
            if (!created)
                status = cxx::unexpected(translate(created.error()));
        }
    };
    SceneConfigurationElement::SceneConfigurationElement(
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        const lux::project::PluginCatalog& catalog,
        const lux::project::SceneRegistrations& registrations,
        std::span<const ConfigurationEditorRegistration> configurations,
        std::span<const SceneProviderOption> providers,
        EditorResult<void>& status
    )
        : Element(parent, id),
          impl_(std::make_unique<Impl>(*this, catalog, registrations, configurations, providers, status))
    {
        setStretch({1, 1});
    }
    SceneConfigurationElement::~SceneConfigurationElement() noexcept = default;
    EditorResult<SceneConfiguration> SceneConfigurationElement::build() noexcept
    {
        auto value = impl_->form_->build();
        if (!value)
            return cxx::unexpected(translate(value.error()));
        return SceneConfiguration{
            std::move(value->name),
            std::move(value->schemas),
            std::move(value->simulation),
            std::move(value->scene),
            value->viewport
        };
    }
    EditorResult<void> SceneConfigurationElement::applyPreset(ESceneContentPreset preset) noexcept
    {
        auto applied = impl_->form_->applyPreset(static_cast<scene::ESceneContentPreset>(preset));
        if (!applied)
            return cxx::unexpected(translate(applied.error()));
        return {};
    }
    void SceneConfigurationElement::setStage(ESceneConfigurationStage stage) noexcept
    {
        impl_->form_->setStage(static_cast<scene::ESceneConfigurationStage>(stage));
    }
    lux::ui::SizeHint SceneConfigurationElement::sizeHintContent() noexcept
    {
        return impl_->form_->sizeHint();
    }
    lux::ui::SizeHint SceneConfigurationElement::measureContent(float width) noexcept
    {
        return impl_->form_->measure(width);
    }
    void SceneConfigurationElement::arrangeContent() noexcept
    {
        impl_->form_->arrange({{}, rect().size});
    }
    void SceneConfigurationElement::draw() noexcept
    {
        drawChild(*impl_->form_);
    }

}
