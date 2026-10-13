#pragma once

#include "DingoNet/Log.h"

#include <format>
#include <utility>

namespace Dingo::Net::Internal
{

	void Log(LogLevel level, std::string_view message);

	template<typename... Args>
	void LogTrace(std::format_string<Args...> format, Args&&... args)
	{
		Log(LogLevel::Trace, std::format(format, std::forward<Args>(args)...));
	}

	template<typename... Args>
	void LogInfo(std::format_string<Args...> format, Args&&... args)
	{
		Log(LogLevel::Info, std::format(format, std::forward<Args>(args)...));
	}

	template<typename... Args>
	void LogWarn(std::format_string<Args...> format, Args&&... args)
	{
		Log(LogLevel::Warn, std::format(format, std::forward<Args>(args)...));
	}

	template<typename... Args>
	void LogError(std::format_string<Args...> format, Args&&... args)
	{
		Log(LogLevel::Error, std::format(format, std::forward<Args>(args)...));
	}

}
