#pragma once
#include <string_view>
#include <memory>

#define LUX_OBJECT_REGIST

namespace lux::meta
{
	class LuxObject
	{
	public:
		[[nodiscard]] virtual bool serializeToFile(std::string_view path) { return false; };
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
