#pragma once
#include <string>
#include <iostream>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/meta/LuxObject.hpp>

double LUX_FUNC() func(float arg1, double arg2);

namespace ns1 {
	std::string func2(float arg1, double arg2);
}

struct LUX_CLASS() TestClass : public lux::meta::EntityObject
{
	TestClass()
	{
		name = "TestClass";
		full_name = "TestClass";
		std::cout << "Test Class Constructor:" << static_cast<uint32_t>(id()) << std::endl;
	}

	~TestClass()
	{
		std::cout << "Test Class Destructor:" << static_cast<uint32_t>(id()) << std::endl;
	}

	int func(int arg1, const std::string& arg2)
	{
		std::cout << arg2 << std::endl;
		return arg1 + 1;
	}

	int func2(int arg1, TestClass* arg2)
	{
		std::cout << "arg2:" << arg2->name << std::endl;
		return arg1 + 1;
	}

	std::string name;
	std::string full_name;
};

enum class LUX_ENUM() TestEnum
{
	ENUM,
	ENUM1,
	ENUM2 = 232
};