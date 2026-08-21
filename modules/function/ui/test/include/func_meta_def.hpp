#pragma once
#include <lux/engine/meta/Meta.hpp>

static inline std::string LUX_FUNC() TestFunc(std::string a, int b)
{
	return a + std::to_string(b);
}

class LUX_CLASS() TestClass
{
public:
	TestClass() = default;

	TestClass(std::string a, int b)
		: a_(std::move(a)), b_(b) {
	}

	std::string testFunc(std::string a, int b)
	{
		return a + std::to_string(b);
	}

private:
	std::string a_;
	int b_;
};
