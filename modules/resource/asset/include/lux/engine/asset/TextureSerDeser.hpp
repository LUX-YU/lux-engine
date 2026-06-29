#pragma once
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>

namespace lux::asset
{
    struct TextureLoadConfig
    {
        bool flip_vertically{ false };
        bool force_alpha{ true };
        lux::rdesc::ETexturePixelFormat output_format{ lux::rdesc::ETexturePixelFormat::RGBA8_SRGB };
        lux::rdesc::ETextureColorSpace color_space{ lux::rdesc::ETextureColorSpace::SRGB };
        uint32_t flags{ lux::rdesc::toUnderlying(lux::rdesc::ETextureAssetFlags::NONE) };

        /// Stable seed for a deterministic (name-based) texture id; empty = random.
        /// ModelSerDeser sets this per-texture before delegating an import so that
        /// re-importing a model reproduces identical texture ids (see
        /// ModelSerDeserConfig::deterministic_seed).
        std::string deterministic_seed;
    };

    class LUX_RESOURCE_PUBLIC TextureSerDeser : public TAssetSerDeser<TextureLoadConfig>
    {
        friend class ModelSerDeser;
    public:
        explicit TextureSerDeser(std::shared_ptr<AssetManager>);

        ~TextureSerDeser() override;

        [[nodiscard]] lux::cxx::expected<AssetIDPair, EAssetError>
        importFromFile(const std::filesystem::path& external_path) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
		fromMemory(const void* mem, size_t len) override;

        /**
         * @brief Decode the PURE Texture data object from a complete .luxasset
         *        memory image, WITHOUT touching the AssetManager.
         *
         * Reads the header + TextureAssetInfo, carves the pixel data section,
         * validates the mip ranges and reconstructs the owning rdesc::Texture.
         * No registration, no AssetInfo, no manager_ access — this is the
         * thread-safe data-only entry point for the async loader: a worker
         * decodes the bytes off the main thread, then the main thread injects
         * the result into an already-existing asset shell.
         *
         * @param bytes Pointer to the first byte of the .luxasset image.
         * @param len   Size of the image in bytes.
         * @return The decoded Texture on success, or an EAssetError. noexcept.
         */
        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
        decodeData(const void* bytes, std::size_t len) noexcept;

    protected:
		EAssetError
        exportAsLuxAssetStream(const LuxAsset& asset, std::ofstream& path) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& path) override;

    private:
        EAssetError loadInfoFromFile(const std::filesystem::path& path, TextureInfo& texture);
    };
}
