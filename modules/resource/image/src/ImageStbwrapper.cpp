#include <lux-engine/resource/image/Image.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace lux::engine::resource
{
	class Image::Impl
	{
	public:
		Impl(std::string path, bool flip_vertically)
		{
			stbi_set_flip_vertically_on_load(flip_vertically);
			load(path.c_str());
		}

		~Impl()
		{
			if (_data)
				stbi_image_free(_data);
		}

		void load(const char *path)
		{
			if (_data)
				stbi_image_free(_data);
			_data = stbi_load(path, &_width, &_height, &_channel, 0);
		}

		bool isEnable() const
		{
			return _data != nullptr;
		}

		int width() const
		{
			return _width;
		}

		int height() const
		{
			return _height;
		}

		int channel() const
		{
			return _channel;
		}

		void *data()
		{
			return _data;
		}
	private:
        void*   _data{nullptr};
        int     _width;
        int     _height;
        int     _channel;
	};

	Image::Image(std::string path, bool flip_vertically)
	{
		_impl = std::make_shared<Impl>(std::move(path), flip_vertically);
	}

	bool Image::isEnable() const
	{
		return _impl->isEnable();
	}

	int Image::width() const
	{
		return _impl->width();
	}

	int Image::height() const
	{
		return _impl->height();
	}

	int Image::channel() const
	{
		return _impl->channel();
	}

	void *Image::data()
	{
		return _impl->data();
	}
} // namespace lux::engine::platform
