#pragma once

#include <lux/cxx/core/StableNameId.hpp>

#include <string_view>

namespace lux::scene
{
    /// Stable identity for one capability installed into a Scene. Scene feature
    /// identity belongs to the Scene domain and is deliberately distinct from
    /// Extension, Render Effect, Editor Panel, and serialized legacy IDs.
    struct SceneFeatureIdTag final {};

    using SceneFeatureIdView = lux::cxx::StableNameIdView<SceneFeatureIdTag>;
    using SceneFeatureId     = lux::cxx::StableNameId<SceneFeatureIdTag>;

    [[nodiscard]] constexpr SceneFeatureIdView sceneFeatureId(std::string_view name) noexcept
    {
        return SceneFeatureIdView{name};
    }

    [[nodiscard]] inline bool isValidSceneFeatureIdName(std::string_view name) noexcept
    {
        if (name.empty() || name.front() == '.' || name.back() == '.')
            return false;

        bool has_dot = false;
        bool previous_dot = false;
        for (const char value : name)
        {
            const bool dot = value == '.';
            if (dot)
            {
                if (previous_dot)
                    return false;
                has_dot = true;
            }
            else if (!((value >= 'a' && value <= 'z') ||
                       (value >= '0' && value <= '9') ||
                       value == '_' || value == '-'))
            {
                return false;
            }
            previous_dot = dot;
        }
        return has_dot;
    }

    [[nodiscard]] inline bool sameSceneFeatureId(SceneFeatureIdView lhs, SceneFeatureIdView rhs) noexcept
    {
        return lhs == rhs;
    }

    [[nodiscard]] inline bool sceneFeatureIdCollision(SceneFeatureIdView lhs, SceneFeatureIdView rhs) noexcept
    {
        return lhs.hash() == rhs.hash() && lhs.name() != rhs.name();
    }
} // namespace lux::scene
