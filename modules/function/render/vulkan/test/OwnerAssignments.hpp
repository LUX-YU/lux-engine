#pragma once

#include <array>
#include <lux/engine/render/vulkan/shader/Layout.hpp>

namespace foundation_test
{
    // Old fixtures deliberately have one owner in each category. Expand their actual
    // generated fields at authoring time; production APIs have no category fallback.
    inline std::vector<lux::render::vulkan::OwnerAssignment> fixtureOwners(
        const lux::rdesc::PassShaderContract& schema,
        std::initializer_list<lux::render::vulkan::ShaderOwnerId> owners
    )
    {
        std::vector<lux::render::vulkan::OwnerAssignment> result;
        for (const auto& field : schema.resources)
        {
            if (lux::rdesc::isShaderDescriptorRole(field.role))
            {
                result.push_back({field.path, owners.begin()[static_cast<unsigned>(field.owner)]});
            }
        }
        return result;
    }
} // namespace foundation_test
