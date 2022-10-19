#include <lux-engine/resource/image/Image.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace lux::engine::resource
{
	Image::Image(std::string path, bool flip_vertically)
	{
		stbi_set_flip_vertically_on_load(flip_vertically);
		load(path.c_str());
	}

	Image::~Image()
	{
		stbi_image_free(_data);
	}

	void Image::load(const char *path)
	{
		if(_data) stbi_image_free(_data);
		_data = stbi_load(path, &_width, &_height, &_channel, 0);
	}

	bool Image::isEnable()
	{
		return _data != nullptr;
	}

	int Image::width()
	{
		return _width;
	}

	int Image::height()
	{
		return _height;
	}

	int Image::channel()
	{
		return _channel;
	}

	void* Image::data()
	{
		return _data;
	}
} // namespace lux::engine::platform
