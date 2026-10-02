#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>

namespace lux::editor::scene
{
    SceneConfigurationResult<ConfigurationControl> makeConfigurationControl(
        const ConfigurationEditor& registration,
        lux::ui::Element& parent,
        lux::ui::ElementId id,
        std::optional<std::span<const std::byte>> initial
    )
    {
        // Capture the entire descriptor/code before calling either reflection or a contributed factory.
        const auto pinned = registration;
        if (!pinned.code.valid() || !pinned.create)
            return cxx::unexpected(
                SceneConfigurationFailure{ESceneConfigurationError::MISSING_PROVIDER, "configuration"}
            );
        auto created = ConfigurationValue::create(pinned.value, std::make_shared<contracts::CodeLease>(pinned.code));
        if (!created)
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::CONTROL_FAILURE,
                "configuration.value",
                static_cast<std::uint64_t>(created.error())
            });
        auto value = std::make_shared<ConfigurationValue>(std::move(*created));
        if (initial)
        {
            const auto decoded = value->decode(*initial);
            if (!decoded)
                return cxx::unexpected(SceneConfigurationFailure{
                    ESceneConfigurationError::CONTROL_FAILURE,
                    "configuration.decode",
                    static_cast<std::uint64_t>(decoded.error().code),
                    {},
                    decoded.error()
                });
        }
        auto element = pinned.create(parent, std::move(id), *value);
        if (!element)
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::CONTROL_FAILURE,
                "configuration.control",
                static_cast<std::uint64_t>(element.error().code),
                element.error().message
            });
        return ConfigurationControl{
            std::move(value),
            std::move(*element),
            [](const void* data, std::vector<std::byte>& bytes) noexcept {
                return static_cast<const ConfigurationValue*>(data)->encode(bytes);
            }
        };
    }
}
