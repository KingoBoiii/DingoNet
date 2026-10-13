#pragma once

#include <vector>

namespace TestFramework
{

	using TestFn = void(*)();

	struct TestCase
	{
		const char* Name;
		TestFn Run;
	};

	std::vector<TestCase>& Registry();
	void ReportFailure(const char* expression, const char* file, int line);
	int RunAll();

	struct Registrar
	{
		Registrar(const char* name, TestFn fn)
		{
			Registry().push_back({ name, fn });
		}
	};

}

#define TEST(name) \
	static void Test_##name(); \
	static ::TestFramework::Registrar s_Registrar_##name(#name, &Test_##name); \
	static void Test_##name()

#define CHECK(cond) \
	do \
	{ \
		if (!(cond)) \
		{ \
			::TestFramework::ReportFailure(#cond, __FILE__, __LINE__); \
		} \
	} while (false)
