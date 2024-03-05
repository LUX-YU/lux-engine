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
		[[nodiscard]] virtual bool serializeToFile(std::string_view path) { return false; };

#if defined __GENERATE_TIME__
		[[nodiscard]] virtual const char* object_name() = 0;

		[[nodiscard]] virtual const std::vector<>
#endif
	};

	template<class _Sub>
	class TLuxObject : public LuxObject
	{
	public:
		[[nodiscard]] static bool TserializeToFile(std::string_view path)
		{
			return _Sub::__TserializeToFile(path);
		}

		[[nodiscard]] static bool TdeserializeFromFile(std::string_view path, _Sub& subclass)
		{
			return _Sub::__TdeserializeFromFile(path, subclass);
		}

		[[nodiscard]] bool serializeToFile(std::string_view path) override
		{
			return TLuxObject<_Sub>::TserializeToFile(path);
		}

	protected:
		static bool __TserializeToFile(std::string_view path) { return false; }
		static bool __TdeserializeFromFile(std::string_view path, _Sub& subclass) { return false; }
	// private:
	// 	std::unique_ptr<LuxObjectImpl> _impl;
	};
}
