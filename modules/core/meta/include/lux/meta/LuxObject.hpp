#pragma once
#include <string_view>
#include <memory>

#if defined __PARSE_TIME__
#include <vector>
#define LUX_META(...) __attribute__((annotate("LUX::META;"#__VA_ARGS__)))
#else
#define LUX_META(...)
#endif

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
		[[nodiscard]] static inline bool TserializeToFile(std::string_view path)
		{
			return _Sub::__TserializeToFile(path);
		}

		[[nodiscard]] static inline bool TdeserializeFromFile(std::string_view path, _Sub& subclass)
		{
			return _Sub::__TdeserializeFromFile(path, subclass);
		}

		[[nodiscard]] bool serializeToFile(std::string_view path) override
		{
			return TLuxObject<_Sub>::TserializeToFile(path);
		}

	protected:
		static inline bool __TserializeToFile(std::string_view path) { return false; }
		static inline bool __TdeserializeFromFile(std::string_view path, _Sub& subclass) { return false; }
	// private:
	// 	std::unique_ptr<LuxObjectImpl> _impl;
	};
}
