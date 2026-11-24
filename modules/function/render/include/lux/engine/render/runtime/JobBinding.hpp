#pragma once

#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/render/resource/ResourceId.hpp>
#include <lux/engine/render/core/FinalBufferVersion.hpp>
#include <lux/engine/render/graph/RasterViewport.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lux::render
{
    struct TargetImageInput final
    {
        TargetId target;
        std::string binding{}; // Named input of an EffectGraph.
        EProductRead read = EProductRead::UNSPECIFIED;
    };

    struct JobResourceInput final
    {
        std::string binding;
        ResourceInstanceId resource;
        std::uint32_t projection = 0; // Index in the Resource's graph binding contract.
        std::optional<FinalBufferVersion> publication;
    };

    struct JobInputs final
    {
        RasterViewport viewport;
        std::vector<std::uint8_t> conditions;
    };

    struct JobBinding final
    {
        JobInputs inputs;
        std::vector<TargetImageInput> target_images;
        std::vector<JobResourceInput> resources{};
    };
} // namespace lux::render
