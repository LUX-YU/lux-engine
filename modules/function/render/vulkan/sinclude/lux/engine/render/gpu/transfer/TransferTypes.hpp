#pragma once
/**
 * @file TransferTypes.hpp
 * @brief Type definitions for the unified transfer subsystem.
 *
 * Resources declare their buffer's read domain via EBufferDomain; the
 * TransferScheduler maps these to the correct Vulkan pipeline stage and
 * access flags for both pre-copy and post-copy barriers.
 *
 * Thread safety: all types are POD / trivially copyable and safe to use
 * from the render thread only.
 */

#include <vulkan/vulkan.h>

#include <cstdint>

struct VmaAllocation_T;
using VmaAllocation = VmaAllocation_T*;

namespace lux::render
{

    // =========================================================================
    //  Staging allocation result
    // =========================================================================

    /// Result of a transient staging-buffer allocation (host-visible, mapped).
    struct StagingAlloc
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{nullptr};
        void* mapped{nullptr};
        VkDeviceSize srcOffset{0}; ///< Offset within buffer (non-zero for ring sub-allocs).

        explicit operator bool() const noexcept
        {
            return mapped != nullptr;
        }
    };

    // =========================================================================
    //  Buffer domain — describes how the GPU reads a buffer after the transfer
    // =========================================================================

    enum class EBufferDomain : uint8_t
    {
        VertexInput,    ///< VBO/IBO — read by VERTEX_INPUT stage
        Storage_VS,     ///< SSBO read by vertex shader
        Storage_CS,     ///< SSBO read by compute shader
        Storage_FS,     ///< SSBO read by fragment shader
        Storage_VS_CS,  ///< SSBO read by vertex + compute
        Storage_VS_FS,  ///< SSBO read by vertex + fragment
        Storage_All,    ///< SSBO read by vertex + compute + fragment
        VertexInput_CS, ///< VBO/SSBO read by vertex input + compute shader
        Sampled_FS,     ///< Image sampled in fragment shader
        TransferDst,    ///< Fresh buffer/image — no prior reader, skip pre-barrier
    };

    // =========================================================================
    //  Domain → Vulkan stage/access mapping
    // =========================================================================

    struct StageAccessPair
    {
        VkPipelineStageFlags2 stages{VK_PIPELINE_STAGE_2_NONE};
        VkAccessFlags2 access{VK_ACCESS_2_NONE};
    };

    inline StageAccessPair domainToStageAccess(EBufferDomain domain) noexcept
    {
        switch (domain)
        {
        case EBufferDomain::VertexInput:
            return {
                VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT};
        case EBufferDomain::Storage_VS:
            return {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Storage_CS:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Storage_FS:
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Storage_VS_CS:
            return {
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Storage_VS_FS:
            return {
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Storage_All:
            return {
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::VertexInput_CS:
            return {
                VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        case EBufferDomain::Sampled_FS:
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        case EBufferDomain::TransferDst:
            return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
        }
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }

    // =========================================================================
    //  Copy requests — submitted by resource classes to TransferScheduler
    // =========================================================================

    struct BufferCopyRequest
    {
        VkBuffer src{VK_NULL_HANDLE};
        VkDeviceSize src_offset{0};
        VkBuffer dst{VK_NULL_HANDLE};
        VkDeviceSize dst_offset{0};
        VkDeviceSize size{0};
        EBufferDomain domain{EBufferDomain::Storage_All};
        int8_t priority{0}; ///< Lower values are recorded first (grow=-1, data=0)
    };

    struct ImageCopyRequest
    {
        VkBuffer src{VK_NULL_HANDLE};
        VkDeviceSize src_offset{0};
        VkImage dst{VK_NULL_HANDLE};
        VkImageLayout old_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkImageLayout new_layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkImageSubresourceLayers subresource{};
        VkOffset3D offset{};
        VkExtent3D extent{};
        EBufferDomain domain{EBufferDomain::Sampled_FS};
    };

    // =========================================================================
    //  Queue Family Ownership Transfer (QFOT) acquire request
    // =========================================================================

    struct QFOTAcquireRequest
    {
        enum class Kind : uint8_t
        {
            Buffer,
            Image
        };
        Kind kind{Kind::Buffer};

        // Buffer QFOT
        VkBuffer buffer{VK_NULL_HANDLE};
        VkDeviceSize buf_offset{0};
        VkDeviceSize buf_size{0};

        // Image QFOT
        VkImage image{VK_NULL_HANDLE};
        VkImageLayout img_layout{VK_IMAGE_LAYOUT_UNDEFINED};
        VkImageSubresourceRange img_range{};

        uint32_t src_family{VK_QUEUE_FAMILY_IGNORED};
        uint32_t dst_family{VK_QUEUE_FAMILY_IGNORED};
        EBufferDomain domain{EBufferDomain::VertexInput};
    };

} // namespace lux::render
