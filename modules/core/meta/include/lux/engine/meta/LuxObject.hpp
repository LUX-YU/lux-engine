#pragma once
#include <string_view>
#include <lux/cxx/dref/parser/Attribute.hpp>

#define LUX_META_CLASS(...) LUX_REF_MARK(class, __VA_ARGS__)
#define LUX_META_FUNC(...)  LUX_REF_MARK(function, __VA_ARGS__)
#define LUX_META_ENUM(...)  LUX_REF_MARK(enum, __VA_ARGS__)
#define LUX_META_VAR(...)   LUX_REF_MARK(var, __VA_ARGS__)

namespace lux::meta
{
	class LuxObject
	{
	public:
		[[nodiscard]] virtual bool serialize(std::string_view path) { return false; };

		[[nodiscard]] virtual void update() {};
	};

	template<class _Sub>
	class TLuxObject : public LuxObject
	{
	public:
		[[nodiscard]] static bool Tserialize(std::string_view path)
		{
			return _Sub::__Tserialize(path);
		}

		[[nodiscard]] static bool Tdeserialize(std::string_view path, _Sub& subclass)
		{
			return _Sub::__Tdeserialize(path, subclass);
		}

		[[nodiscard]] bool serialize(std::string_view path) override
		{
			return TLuxObject<_Sub>::Tserialize(path);
		}

	protected:
		static bool __Tserialize(std::string_view path) { return false; }
		static bool __Tdeserialize(std::string_view path, _Sub& subclass) { return false; }
	// private:
	// 	std::unique_ptr<LuxObjectImpl> _impl;
	};
}
