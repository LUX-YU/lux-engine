#pragma once
#include "Support.hpp"
#include <filesystem>
#include <fstream>
#include <lux/engine/render/vulkan/shader/Program.hpp>
#include <vector>

namespace shader_test
{
    inline void saveProgram(
        const lux::render::vulkan::NativeShaderProgram& program,
        const std::filesystem::path& directory,
        const std::string& name
    )
    {
        std::filesystem::create_directories(directory);
        const auto& identity = program.identity();
        std::ofstream mapping(directory / (name + ".layout.txt"));
        CHECK(mapping);
        mapping << "device " << identity.vendor << ' ' << identity.device << ' ' << identity.driver << ' '
                << identity.api << '\n';
        for (const auto& owner : identity.layout.owners)
        {
            mapping << "owner " << owner.canonical_name << ' ' << owner.identity.value() << ' ' << owner.revision
                    << '\n';
            for (const auto& field : owner.fields)
            {
                mapping << "full-field " << field.semantic << ' ' << field.type << ' ' << field.count << ' '
                        << field.stages << ' ' << field.element_stride << ' ' << field.element_alignment << '\n';
            }
        }
        for (const auto& field : identity.layout.fields)
        {
            mapping << "relocation " << field.field_index << ' ' << field.owner.value() << ' ' << field.set << ' '
                    << field.binding << ' ' << field.type << ' ' << field.count << ' ' << field.stages << ' '
                    << field.dynamic_index << '\n';
        }
        for (const auto& range : identity.layout.push_ranges)
        {
            mapping << "push " << range.offset << ' ' << range.size << ' ' << range.stages << '\n';
        }
        for (const auto& stage : identity.relocated)
        {
            std::ofstream binary(
                directory / (name + ".stage-" + std::to_string(stage.stage) + ".relocated.spv"),
                std::ios::binary
            );
            binary.write(reinterpret_cast<const char*>(stage.words.data()), stage.words.size() * sizeof(std::uint32_t));
            CHECK(binary);
        }
    }

    inline std::vector<std::uint32_t> words(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        CHECK(input && input.tellg() > 0 && input.tellg() % 4 == 0);
        const auto size = static_cast<std::size_t>(input.tellg());
        std::vector<std::uint32_t> result(size / 4);
        input.seekg(0);
        input.read(reinterpret_cast<char*>(result.data()), size);
        CHECK(input);
        return result;
    }

    inline std::string text(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        CHECK(input);
        return {std::istreambuf_iterator<char>(input), {}};
    }

    // Test-owned one-command transitions only; not Graph synchronization compilation.
    inline void imageBarrier(
        VkCommandBuffer command,
        VkImage image,
        VkImageLayout before,
        VkImageLayout after,
        VkPipelineStageFlags2 source_stage,
        VkAccessFlags2 source_access,
        VkPipelineStageFlags2 destination_stage,
        VkAccessFlags2 destination_access,
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT
    )
    {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = source_stage;
        barrier.srcAccessMask = source_access;
        barrier.dstStageMask = destination_stage;
        barrier.dstAccessMask = destination_access;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    inline void memoryBarrier(
        VkCommandBuffer command,
        VkPipelineStageFlags2 source_stage,
        VkAccessFlags2 source_access,
        VkPipelineStageFlags2 destination_stage,
        VkAccessFlags2 destination_access
    )
    {
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = source_stage;
        barrier.srcAccessMask = source_access;
        barrier.dstStageMask = destination_stage;
        barrier.dstAccessMask = destination_access;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }
} // namespace shader_test
