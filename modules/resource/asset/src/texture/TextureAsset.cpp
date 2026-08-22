#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

namespace lux::asset
{
/**
 * @brief Constructs a TextureAsset with the given asset information.
 * @param info Unique pointer to AssetInfo containing texture metadata
 */
TextureAsset::TextureAsset(std::unique_ptr<AssetInfo> info)
: TAsset<lux::rdesc::Texture>(std::move(info)) {}

/**
 * @brief Move constructor for TextureAsset.
 */
TextureAsset::TextureAsset(TextureAsset&& other) noexcept = default;

/**
 * @brief Move assignment operator for TextureAsset.
 * @return Reference to this TextureAsset
 */
TextureAsset& TextureAsset::operator=(TextureAsset&& other) noexcept = default;

/**
 * @brief Destructor for TextureAsset.
 */
TextureAsset::~TextureAsset() {}
} // namespace lux::asset
