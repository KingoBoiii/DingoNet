#include "TestFramework.h"

#include "Internal/Log.h"

#include <string>

namespace
{

	int s_Calls = 0;
	Dingo::Net::LogLevel s_LastLevel = Dingo::Net::LogLevel::Trace;
	std::string s_LastMessage;

	void CaptureHandler(Dingo::Net::LogLevel level, std::string_view message)
	{
		s_Calls++;
		s_LastLevel = level;
		s_LastMessage = std::string(message);
	}

}

TEST(LogHandlerReceivesLevelAndMessage)
{
	s_Calls = 0;
	Dingo::Net::SetLogHandler(&CaptureHandler);
	Dingo::Net::Internal::LogWarn("peer {} dropped after {} ms", 3, 250);
	CHECK(s_Calls == 1);
	CHECK(s_LastLevel == Dingo::Net::LogLevel::Warn);
	CHECK(s_LastMessage == "peer 3 dropped after 250 ms");

	Dingo::Net::Internal::LogError("bad");
	CHECK(s_Calls == 2);
	CHECK(s_LastLevel == Dingo::Net::LogLevel::Error);
	Dingo::Net::SetLogHandler(nullptr);
}

TEST(LogNullHandlerRestoresDefault)
{
	s_Calls = 0;
	Dingo::Net::SetLogHandler(&CaptureHandler);
	Dingo::Net::SetLogHandler(nullptr);
	Dingo::Net::Internal::LogInfo("default handler line");
	CHECK(s_Calls == 0);
}
