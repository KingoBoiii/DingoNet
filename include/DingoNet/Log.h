#pragma once

#include <string_view>

namespace Dingo::Net
{

	enum class LogLevel
	{
		Trace,
		Info,
		Warn,
		Error
	};

	using LogHandler = void(*)(LogLevel level, std::string_view message);

	void SetLogHandler(LogHandler handler);

}
