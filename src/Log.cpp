#include "Internal/Log.h"

#include <atomic>
#include <cstdio>

namespace Dingo::Net
{

	namespace
	{

		void DefaultHandler(LogLevel level, std::string_view message)
		{
			const char* name = "TRACE";
			switch (level)
			{
			case LogLevel::Trace: name = "TRACE"; break;
			case LogLevel::Info: name = "INFO"; break;
			case LogLevel::Warn: name = "WARN"; break;
			case LogLevel::Error: name = "ERROR"; break;
			}

			std::fprintf(stderr, "[DingoNet] %s: %.*s\n", name, static_cast<int>(message.size()), message.data());
		}

		std::atomic<LogHandler> s_Handler{ &DefaultHandler };

	}

	void SetLogHandler(LogHandler handler)
	{
		s_Handler.store(handler ? handler : &DefaultHandler);
	}

	namespace Internal
	{

		void Log(LogLevel level, std::string_view message)
		{
			s_Handler.load()(level, message);
		}

	}

}
