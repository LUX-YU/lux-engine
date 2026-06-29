#include <lux/engine/editor/thumbnail/ImageCodec.hpp>

// stb_image is ALSO compiled (non-static) inside the asset module. Build our
// copy STATIC so the symbols stay file-local and don't collide at link time.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <cstring>
#include <utility>

namespace lux::editor
{
    std::vector<std::byte>
    encodePngRgba8(const std::byte* rgba, std::uint32_t w, std::uint32_t h)
    {
        std::vector<std::byte> out;
        if (!rgba || w == 0 || h == 0)
            return out;

        const auto write_cb = [](void* ctx, void* data, int size)
        {
            auto* v = static_cast<std::vector<std::byte>*>(ctx);
            const auto* p = static_cast<const std::byte*>(data);
            v->insert(v->end(), p, p + size);
        };
        const int stride = static_cast<int>(w) * 4;
        if (stbi_write_png_to_func(write_cb, &out,
                                   static_cast<int>(w), static_cast<int>(h),
                                   4, rgba, stride) == 0)
            out.clear();
        return out;
    }

    std::optional<DecodedImage>
    decodePngRgba8(std::span<const std::byte> data)
    {
        if (data.empty())
            return std::nullopt;

        int w = 0, h = 0, comp = 0;
        unsigned char* pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(data.data()),
            static_cast<int>(data.size()), &w, &h, &comp, 4);
        if (!pixels)
            return std::nullopt;

        DecodedImage img;
        img.width  = static_cast<std::uint32_t>(w);
        img.height = static_cast<std::uint32_t>(h);
        const std::size_t n = static_cast<std::size_t>(w) * h * 4;
        img.rgba8.resize(n);
        std::memcpy(img.rgba8.data(), pixels, n);
        stbi_image_free(pixels);
        return img;
    }

    std::vector<std::byte>
    downscaleRgba8(const std::byte* src, std::uint32_t sw, std::uint32_t sh,
                   std::uint32_t dw, std::uint32_t dh)
    {
        std::vector<std::byte> out;
        if (!src || sw == 0 || sh == 0 || dw == 0 || dh == 0)
            return out;

        out.resize(static_cast<std::size_t>(dw) * dh * 4);
        unsigned char* r = stbir_resize_uint8_srgb(
            reinterpret_cast<const unsigned char*>(src),
            static_cast<int>(sw), static_cast<int>(sh), 0,
            reinterpret_cast<unsigned char*>(out.data()),
            static_cast<int>(dw), static_cast<int>(dh), 0,
            STBIR_RGBA);
        if (!r)
            out.clear();
        return out;
    }

    void swizzleBgraRgba(std::byte* pixels, std::size_t pixel_count)
    {
        for (std::size_t i = 0; i < pixel_count; ++i)
        {
            std::byte* p = pixels + i * 4;
            std::swap(p[0], p[2]); // B <-> R
        }
    }

} // namespace lux::editor
