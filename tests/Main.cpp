#include "TestFramework.h"

#include <cstdio>

namespace TestFramework
{

	namespace
	{
		int s_CurrentFailures = 0;
	}

	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> s_Registry;
		return s_Registry;
	}

	void ReportFailure(const char* expression, const char* file, int line)
	{
		std::printf("  %s:%d: CHECK(%s) failed\n", file, line, expression);
		s_CurrentFailures++;
	}

	int RunAll()
	{
		int failed = 0;
		for (const TestCase& test : Registry())
		{
			s_CurrentFailures = 0;
			test.Run();
			if (s_CurrentFailures == 0)
			{
				std::printf("PASS %s\n", test.Name);
			}
			else
			{
				std::printf("FAIL %s\n", test.Name);
				failed++;
			}
		}

		const int total = static_cast<int>(Registry().size());
		std::printf("%d passed, %d failed, %d total\n", total - failed, failed, total);
		return failed == 0 ? 0 : 1;
	}

}

int main()
{
	return TestFramework::RunAll();
}
