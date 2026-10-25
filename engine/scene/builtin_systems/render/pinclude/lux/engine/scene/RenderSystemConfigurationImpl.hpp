#pragma once

#include <lux/engine/scene/SceneSystemRegistration.hpp>

namespace lux::scene::detail
{
    [[nodiscard]] std::span<const SceneConfigurationMigration> renderConfigurationMigrations() noexcept;
}
