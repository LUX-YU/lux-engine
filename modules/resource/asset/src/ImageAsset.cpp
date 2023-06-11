#include <lux/engine/asset/ImageAsset.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace lux::asset
{
	ImageAsset::ImageAsset(FilePath path, bool flip_vertically)
	: LuxExternalAsset(std::move(path))
	{
		_is_flip_vertically = flip_vertically;
	}

	ImageAsset::~ImageAsset()
	{
		if (_data)
			stbi_image_free(_data);
	}

	LoadAssetResult ImageAsset::load()
	{
		if (_data)
			stbi_image_free(_data);

		stbi_set_flip_vertically_on_load(_is_flip_vertically);
		_data = stbi_load(filePath().string().c_str(), &_width, &_height, &_channel, 0);

		return _data != nullptr ? LoadAssetResult::SUCCESS : LoadAssetResult::UNKNOWN_ERROR;
	}

	bool ImageAsset::unload()
	{
		if (_data)
			stbi_image_free(_data);
		_data = nullptr;
		return true;
	}

	bool ImageAsset::isLoaded() const
	{
		return _data != nullptr;
	}

	int ImageAsset::width() const
	{
		return _width;
	}

	int ImageAsset::height() const
	{
		return _height;
	}

	int ImageAsset::channel() const
	{
		return _channel;
	}

	const void* const ImageAsset::data() const
	{
		return _data;
	}

	void* ImageAsset::data()
	{
		return _data;
	}
} // namespace lux::engine::platform
