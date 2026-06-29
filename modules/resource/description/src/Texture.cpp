#include <lux/engine/description/Texture.hpp>
#include <cstdlib>
#include <cstring>

namespace lux::rdesc
{
	/**
	 * @brief Default constructor for Texture.
	 * Initializes texture with zero dimensions and null data.
	 */
	Texture::Texture()
	{
		info_ = {};
		info_.width = 0;
		info_.height = 0;
		info_.channel = 0;
		info_.pixel_format = ETexturePixelFormat::UNKNOWN;
		info_.color_space = ETextureColorSpace::UNKNOWN;
		info_.layers = 1;
		info_.mip_count = 0;
		info_.flags = toUnderlying(ETextureAssetFlags::NONE);
		data_ = nullptr;
		size_ = 0;
		owns_data_ = false;
	}

	/**
	 * @brief Constructs a Texture with the given information and data.
	 * @param info Texture information structure
	 * @param data Pointer to the texture pixel data
	 * @param size Size of the texture data in bytes
	 */
	Texture::Texture(const TextureInfo& info, void* data, size_t size)
		: info_(info), data_(data), size_(size)
	{
		owns_data_ = info.owns_data;
		if(info.copy && data != nullptr && size > 0) {
			void* new_data = std::malloc(size);
			if (new_data) {
				std::memcpy(new_data, data, size);
				data_ = new_data;
			}
		}
	}

	/**
	 * @brief Copy constructor for Texture.
	 * Deep-copies the pixel buffer from another texture.
	 * @param other The texture to copy from
	 */
	Texture::Texture(const Texture& other)
		: info_(other.info_), data_(nullptr), size_(other.size_), owns_data_(other.owns_data_)
	{
		if (other.size_ > 0 && other.data_) {
			data_ = std::malloc(other.size_);
			if (data_) {
				std::memcpy(data_, other.data_, other.size_);
			}
		}
	}

	/**
	 * @brief Copy assignment operator for Texture.
	 * @param other The texture to copy from
	 * @return Reference to this texture
	 */
	Texture& Texture::operator=(const Texture& other)
	{
		if (this != &other) {
			if (data_ && owns_data_) {
				std::free(data_);
			}
			info_ = other.info_;
			size_ = other.size_;
			owns_data_ = other.owns_data_;
			data_ = nullptr;
			if (other.size_ > 0 && other.data_) {
				data_ = std::malloc(other.size_);
				if (data_) {
					std::memcpy(data_, other.data_, other.size_);
				}
			}
		}
		return *this;
	}

	/**
	 * @brief Move constructor for Texture.
	 * Transfers ownership of texture data from another texture.
	 * @param other The texture to move from
	 */
	Texture::Texture(Texture&& other) noexcept
	{
		info_ = other.info_;
		data_ = other.data_;
		size_ = other.size_;
		owns_data_ = other.owns_data_;
		other.info_ = {};
		other.data_ = nullptr;
		other.size_ = 0;
		other.owns_data_ = false;
	}

	/**
	 * @brief Move assignment operator for Texture.
	 * Transfers ownership of texture data from another texture.
	 * @param other The texture to move from
	 * @return Reference to this texture
	 */
	Texture& Texture::operator=(Texture&& other) noexcept
	{
		if (this != &other) {
			if (data_ && owns_data_) {
				std::free(data_);
			}
			info_ = other.info_;
			data_ = other.data_;
			size_ = other.size_;
			owns_data_ = other.owns_data_;
			other.info_ = {};
			other.data_ = nullptr;
			other.size_ = 0;
			other.owns_data_ = false;
		}
		return *this;
	}

	/**
	 * @brief Destructor for Texture.
	 * Frees owned pixel data.
	 */
	Texture::~Texture()
	{
		if (data_ && owns_data_) {
			std::free(data_);
		}
	}
} // namespace lux::rdesc
