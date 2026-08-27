#pragma once
#include <lux/engine/description/Image.hpp> // ETextureFormat(本表唯一所需;不再借道 graph 消 utils→graph 环)
#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <optional>

namespace lux::render
{
    // ── VkFormat -> lux::rdesc::ETextureFormat(唯一权威表) ────────────────
    //
    // 本表原先只有 10 条(kVkFormatMap + mapVkFormat/tryMapVkFormat),而
    // graph/vk_type_converter.hpp 里另有一个覆盖 70 种格式的 convertVkFormat
    // 做同向映射 —— 两者同向、窄表是宽表的严格子集,且宽表**零调用点**
    // (git log -S 显示自首次提交起从未被调用)。现合并:宽表内容搬到这里成为
    // 唯一权威表,宽表删除。
    //
    // 同时去掉了旧 mapVkFormat 的默认 fallback(RGBA8_UNORM)。fallback 把
    // "未知格式"变成一个**合法但错误**的值,能绕过 RenderPassPlanner 的
    // VK_FORMAT_UNDEFINED 报错门,一路烙进 VkPipeline —— 症状是 dynamic
    // rendering 格式失配(通道错序/色彩错乱),且无任何诊断。返回 optional
    // 强制调用方显式处理,与 readbackBpp 的 fail-safe 风格对齐。
    //
    // 已知缺口(合并后依然不覆盖,非本次引入):
    //   VK_FORMAT_A2B10G10R10_UNORM_PACK32 —— HDR swapchain 的首选格式
    //   (gapi Swapchain.hpp chooseSwapSurfaceFormat)。目前不可达,仅因
    //   SwapchainProvider::Config::enable_hdr 默认 false 且全仓无人置真;
    //   开关一翻即会命中 nullopt 分支 —— 现在会得到具名报错而非静默错画。
    [[nodiscard]] inline constexpr std::optional<lux::rdesc::ETextureFormat> tryMapVkFormat(VkFormat format) noexcept
    {
        switch (format)
        {
        case VK_FORMAT_R8_UNORM:
            return lux::rdesc::ETextureFormat::R8_UNORM;
        case VK_FORMAT_R8_SNORM:
            return lux::rdesc::ETextureFormat::R8_SNORM;
        case VK_FORMAT_R8_UINT:
            return lux::rdesc::ETextureFormat::R8_UINT;
        case VK_FORMAT_R8_SINT:
            return lux::rdesc::ETextureFormat::R8_SINT;
        case VK_FORMAT_R8_SRGB:
            return lux::rdesc::ETextureFormat::R8_SRGB;
        case VK_FORMAT_R16_UNORM:
            return lux::rdesc::ETextureFormat::R16_UNORM;
        case VK_FORMAT_R16_SNORM:
            return lux::rdesc::ETextureFormat::R16_SNORM;
        case VK_FORMAT_R16_UINT:
            return lux::rdesc::ETextureFormat::R16_UINT;
        case VK_FORMAT_R16_SINT:
            return lux::rdesc::ETextureFormat::R16_SINT;
        case VK_FORMAT_R16_SFLOAT:
            return lux::rdesc::ETextureFormat::R16_SFLOAT;
        case VK_FORMAT_R32_UINT:
            return lux::rdesc::ETextureFormat::R32_UINT;
        case VK_FORMAT_R32_SINT:
            return lux::rdesc::ETextureFormat::R32_SINT;
        case VK_FORMAT_R32_SFLOAT:
            return lux::rdesc::ETextureFormat::R32_SFLOAT;
        case VK_FORMAT_R8G8_UNORM:
            return lux::rdesc::ETextureFormat::RG8_UNORM;
        case VK_FORMAT_R8G8_SNORM:
            return lux::rdesc::ETextureFormat::RG8_SNORM;
        case VK_FORMAT_R8G8_UINT:
            return lux::rdesc::ETextureFormat::RG8_UINT;
        case VK_FORMAT_R8G8_SINT:
            return lux::rdesc::ETextureFormat::RG8_SINT;
        case VK_FORMAT_R8G8_SRGB:
            return lux::rdesc::ETextureFormat::RG8_SRGB;
        case VK_FORMAT_R16G16_UNORM:
            return lux::rdesc::ETextureFormat::RG16_UNORM;
        case VK_FORMAT_R16G16_SNORM:
            return lux::rdesc::ETextureFormat::RG16_SNORM;
        case VK_FORMAT_R16G16_UINT:
            return lux::rdesc::ETextureFormat::RG16_UINT;
        case VK_FORMAT_R16G16_SINT:
            return lux::rdesc::ETextureFormat::RG16_SINT;
        case VK_FORMAT_R16G16_SFLOAT:
            return lux::rdesc::ETextureFormat::RG16_SFLOAT;
        case VK_FORMAT_R32G32_UINT:
            return lux::rdesc::ETextureFormat::RG32_UINT;
        case VK_FORMAT_R32G32_SINT:
            return lux::rdesc::ETextureFormat::RG32_SINT;
        case VK_FORMAT_R32G32_SFLOAT:
            return lux::rdesc::ETextureFormat::RG32_SFLOAT;
        case VK_FORMAT_R8G8B8_UNORM:
            return lux::rdesc::ETextureFormat::RGB8_UNORM;
        case VK_FORMAT_R8G8B8_SNORM:
            return lux::rdesc::ETextureFormat::RGB8_SNORM;
        case VK_FORMAT_R8G8B8_UINT:
            return lux::rdesc::ETextureFormat::RGB8_UINT;
        case VK_FORMAT_R8G8B8_SINT:
            return lux::rdesc::ETextureFormat::RGB8_SINT;
        case VK_FORMAT_R8G8B8_SRGB:
            return lux::rdesc::ETextureFormat::RGB8_SRGB;
        case VK_FORMAT_R32G32B32_UINT:
            return lux::rdesc::ETextureFormat::RGB32_UINT;
        case VK_FORMAT_R32G32B32_SINT:
            return lux::rdesc::ETextureFormat::RGB32_SINT;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return lux::rdesc::ETextureFormat::RGB32_SFLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return lux::rdesc::ETextureFormat::RGBA8_UNORM;
        case VK_FORMAT_R8G8B8A8_SNORM:
            return lux::rdesc::ETextureFormat::RGBA8_SNORM;
        case VK_FORMAT_R8G8B8A8_UINT:
            return lux::rdesc::ETextureFormat::RGBA8_UINT;
        case VK_FORMAT_R8G8B8A8_SINT:
            return lux::rdesc::ETextureFormat::RGBA8_SINT;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return lux::rdesc::ETextureFormat::RGBA8_SRGB;
        case VK_FORMAT_R16G16B16A16_UNORM:
            return lux::rdesc::ETextureFormat::RGBA16_UNORM;
        case VK_FORMAT_R16G16B16A16_SNORM:
            return lux::rdesc::ETextureFormat::RGBA16_SNORM;
        case VK_FORMAT_R16G16B16A16_UINT:
            return lux::rdesc::ETextureFormat::RGBA16_UINT;
        case VK_FORMAT_R16G16B16A16_SINT:
            return lux::rdesc::ETextureFormat::RGBA16_SINT;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return lux::rdesc::ETextureFormat::RGBA16_SFLOAT;
        case VK_FORMAT_R32G32B32A32_UINT:
            return lux::rdesc::ETextureFormat::RGBA32_UINT;
        case VK_FORMAT_R32G32B32A32_SINT:
            return lux::rdesc::ETextureFormat::RGBA32_SINT;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return lux::rdesc::ETextureFormat::RGBA32_SFLOAT;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return lux::rdesc::ETextureFormat::BGRA8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return lux::rdesc::ETextureFormat::BGRA8_SRGB;
        case VK_FORMAT_D16_UNORM:
            return lux::rdesc::ETextureFormat::D16_UNORM;
        case VK_FORMAT_D32_SFLOAT:
            return lux::rdesc::ETextureFormat::D32_SFLOAT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return lux::rdesc::ETextureFormat::D16_UNORM_S8_UINT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return lux::rdesc::ETextureFormat::D32_SFLOAT_S8_UINT;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC1_RGB_UNORM;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            return lux::rdesc::ETextureFormat::BC1_RGB_SRGB;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC1_RGBA_UNORM;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            return lux::rdesc::ETextureFormat::BC1_RGBA_SRGB;
        case VK_FORMAT_BC2_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC2_UNORM;
        case VK_FORMAT_BC2_SRGB_BLOCK:
            return lux::rdesc::ETextureFormat::BC2_SRGB;
        case VK_FORMAT_BC3_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC3_UNORM;
        case VK_FORMAT_BC3_SRGB_BLOCK:
            return lux::rdesc::ETextureFormat::BC3_SRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC4_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC4_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC5_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC5_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
            return lux::rdesc::ETextureFormat::BC6H_UFLOAT;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
            return lux::rdesc::ETextureFormat::BC6H_SFLOAT;
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return lux::rdesc::ETextureFormat::BC7_UNORM;
        case VK_FORMAT_BC7_SRGB_BLOCK:
            return lux::rdesc::ETextureFormat::BC7_SRGB;
        case VK_FORMAT_UNDEFINED:
        default:
            // 未知格式 -> nullopt。调用方必须显式处理:静默回落到某个"合法但错误"
            // 的格式会绕过 RenderPassPlanner 的 VK_FORMAT_UNDEFINED 报错门,
            // 一路把错格式烙进 VkPipeline。(格式表合并)
            return std::nullopt;
        }
    }

    /// Exact tightly-packed byte size of one (w×h) mip level in VkFormat `f`, BC-block
    /// aligned. Returns 0 for a zero extent, an overflowing size, or a format the upload
    /// path does not know. Mirror of pixelFormatMipBytes() (RenderTypes.hpp, keyed on
    /// EPixelFormat) but keyed on the VkFormat a bindless slot stores, so texture UPDATES
    /// can validate exact byte counts against an EXISTING slot's format.
    [[nodiscard]] inline constexpr std::uint64_t vkFormatMipBytes(VkFormat f, std::uint32_t w, std::uint32_t h) noexcept
    {
        std::uint32_t block_bytes = 0, bw = 1, bh = 1;
        switch (f)
        {
        case VK_FORMAT_R8_UNORM:
            block_bytes = 1;
            break;
        case VK_FORMAT_R8G8_UNORM:
            block_bytes = 2;
            break;
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_UNORM:
            block_bytes = 2;
            break;
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            block_bytes = 4;
            break;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            block_bytes = 8;
            break;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            block_bytes = 16;
            break;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            block_bytes = 8;
            bw = 4;
            bh = 4;
            break;
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
            block_bytes = 16;
            bw = 4;
            bh = 4;
            break;
        default:
            return 0;
        }
        if (w == 0 || h == 0)
            return 0;
        const std::uint64_t bx = (static_cast<std::uint64_t>(w) + bw - 1) / bw;
        const std::uint64_t by = (static_cast<std::uint64_t>(h) + bh - 1) / bh;
        const std::uint64_t blocks = bx * by;
        if (blocks > UINT64_MAX / block_bytes)
            return 0;
        return blocks * block_bytes;
    }

    /// lux::rdesc::ETextureFormat -> VkFormat。原住 graph/vk_type_converter.hpp,
    /// 但它只是纯格式映射、与渲染图无关;留在 graph 会迫使 pipeline 反向依赖
    /// graph(pipeline->graph 环)。移到格式映射表的真正归属地。(分层步骤2b)
    inline VkFormat convertTextureFormat(lux::rdesc::ETextureFormat format)
    {
        switch (format)
        {
        case lux::rdesc::ETextureFormat::R8_UNORM:
            return VK_FORMAT_R8_UNORM;
        case lux::rdesc::ETextureFormat::R8_SNORM:
            return VK_FORMAT_R8_SNORM;
        case lux::rdesc::ETextureFormat::R8_UINT:
            return VK_FORMAT_R8_UINT;
        case lux::rdesc::ETextureFormat::R8_SINT:
            return VK_FORMAT_R8_SINT;
        case lux::rdesc::ETextureFormat::R8_SRGB:
            return VK_FORMAT_R8_SRGB;
        case lux::rdesc::ETextureFormat::R16_UNORM:
            return VK_FORMAT_R16_UNORM;
        case lux::rdesc::ETextureFormat::R16_SNORM:
            return VK_FORMAT_R16_SNORM;
        case lux::rdesc::ETextureFormat::R16_UINT:
            return VK_FORMAT_R16_UINT;
        case lux::rdesc::ETextureFormat::R16_SINT:
            return VK_FORMAT_R16_SINT;
        case lux::rdesc::ETextureFormat::R16_SFLOAT:
            return VK_FORMAT_R16_SFLOAT;
        case lux::rdesc::ETextureFormat::R32_UINT:
            return VK_FORMAT_R32_UINT;
        case lux::rdesc::ETextureFormat::R32_SINT:
            return VK_FORMAT_R32_SINT;
        case lux::rdesc::ETextureFormat::R32_SFLOAT:
            return VK_FORMAT_R32_SFLOAT;
        case lux::rdesc::ETextureFormat::RG8_UNORM:
            return VK_FORMAT_R8G8_UNORM;
        case lux::rdesc::ETextureFormat::RG8_SNORM:
            return VK_FORMAT_R8G8_SNORM;
        case lux::rdesc::ETextureFormat::RG8_UINT:
            return VK_FORMAT_R8G8_UINT;
        case lux::rdesc::ETextureFormat::RG8_SINT:
            return VK_FORMAT_R8G8_SINT;
        case lux::rdesc::ETextureFormat::RG8_SRGB:
            return VK_FORMAT_R8G8_SRGB;
        case lux::rdesc::ETextureFormat::RG16_UNORM:
            return VK_FORMAT_R16G16_UNORM;
        case lux::rdesc::ETextureFormat::RG16_SNORM:
            return VK_FORMAT_R16G16_SNORM;
        case lux::rdesc::ETextureFormat::RG16_UINT:
            return VK_FORMAT_R16G16_UINT;
        case lux::rdesc::ETextureFormat::RG16_SINT:
            return VK_FORMAT_R16G16_SINT;
        case lux::rdesc::ETextureFormat::RG16_SFLOAT:
            return VK_FORMAT_R16G16_SFLOAT;
        case lux::rdesc::ETextureFormat::RG32_UINT:
            return VK_FORMAT_R32G32_UINT;
        case lux::rdesc::ETextureFormat::RG32_SINT:
            return VK_FORMAT_R32G32_SINT;
        case lux::rdesc::ETextureFormat::RG32_SFLOAT:
            return VK_FORMAT_R32G32_SFLOAT;
        case lux::rdesc::ETextureFormat::RGB8_UNORM:
            return VK_FORMAT_R8G8B8_UNORM;
        case lux::rdesc::ETextureFormat::RGB8_SNORM:
            return VK_FORMAT_R8G8B8_SNORM;
        case lux::rdesc::ETextureFormat::RGB8_UINT:
            return VK_FORMAT_R8G8B8_UINT;
        case lux::rdesc::ETextureFormat::RGB8_SINT:
            return VK_FORMAT_R8G8B8_SINT;
        case lux::rdesc::ETextureFormat::RGB8_SRGB:
            return VK_FORMAT_R8G8B8_SRGB;
        case lux::rdesc::ETextureFormat::RGB32_UINT:
            return VK_FORMAT_R32G32B32_UINT;
        case lux::rdesc::ETextureFormat::RGB32_SINT:
            return VK_FORMAT_R32G32B32_SINT;
        case lux::rdesc::ETextureFormat::RGB32_SFLOAT:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case lux::rdesc::ETextureFormat::RGBA8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case lux::rdesc::ETextureFormat::RGBA8_SNORM:
            return VK_FORMAT_R8G8B8A8_SNORM;
        case lux::rdesc::ETextureFormat::RGBA8_UINT:
            return VK_FORMAT_R8G8B8A8_UINT;
        case lux::rdesc::ETextureFormat::RGBA8_SINT:
            return VK_FORMAT_R8G8B8A8_SINT;
        case lux::rdesc::ETextureFormat::RGBA8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case lux::rdesc::ETextureFormat::RGBA16_UNORM:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case lux::rdesc::ETextureFormat::RGBA16_SNORM:
            return VK_FORMAT_R16G16B16A16_SNORM;
        case lux::rdesc::ETextureFormat::RGBA16_UINT:
            return VK_FORMAT_R16G16B16A16_UINT;
        case lux::rdesc::ETextureFormat::RGBA16_SINT:
            return VK_FORMAT_R16G16B16A16_SINT;
        case lux::rdesc::ETextureFormat::RGBA16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case lux::rdesc::ETextureFormat::RGBA32_UINT:
            return VK_FORMAT_R32G32B32A32_UINT;
        case lux::rdesc::ETextureFormat::RGBA32_SINT:
            return VK_FORMAT_R32G32B32A32_SINT;
        case lux::rdesc::ETextureFormat::RGBA32_SFLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case lux::rdesc::ETextureFormat::BGRA8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case lux::rdesc::ETextureFormat::BGRA8_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case lux::rdesc::ETextureFormat::D16_UNORM:
            return VK_FORMAT_D16_UNORM;
        case lux::rdesc::ETextureFormat::D32_SFLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case lux::rdesc::ETextureFormat::D16_UNORM_S8_UINT:
            return VK_FORMAT_D16_UNORM_S8_UINT;
        case lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case lux::rdesc::ETextureFormat::D32_SFLOAT_S8_UINT:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case lux::rdesc::ETextureFormat::BC1_RGB_UNORM:
            return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC1_RGB_SRGB:
            return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case lux::rdesc::ETextureFormat::BC1_RGBA_UNORM:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC1_RGBA_SRGB:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case lux::rdesc::ETextureFormat::BC2_UNORM:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC2_SRGB:
            return VK_FORMAT_BC2_SRGB_BLOCK;
        case lux::rdesc::ETextureFormat::BC3_UNORM:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC3_SRGB:
            return VK_FORMAT_BC3_SRGB_BLOCK;
        case lux::rdesc::ETextureFormat::BC4_UNORM:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC4_SNORM:
            return VK_FORMAT_BC4_SNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC5_UNORM:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC5_SNORM:
            return VK_FORMAT_BC5_SNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC6H_UFLOAT:
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case lux::rdesc::ETextureFormat::BC6H_SFLOAT:
            return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case lux::rdesc::ETextureFormat::BC7_UNORM:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        case lux::rdesc::ETextureFormat::BC7_SRGB:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case lux::rdesc::ETextureFormat::UNDEFINED:
            return VK_FORMAT_UNDEFINED;
        default:
            assert(false && "Unknown lux::rdesc::ETextureFormat");
            return VK_FORMAT_UNDEFINED;
        }
    }

} // namespace lux::render
