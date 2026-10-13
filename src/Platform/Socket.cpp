#include "Platform/Socket.h"

#include <format>

namespace Dingo::Net::Internal
{

	const char* ToString(SocketError error)
	{
		switch (error)
		{
		case SocketError::None: return "no error";
		case SocketError::WouldBlock: return "would block";
		case SocketError::InProgress: return "in progress";
		case SocketError::Refused: return "connection refused";
		case SocketError::Reset: return "connection reset";
		case SocketError::Aborted: return "connection aborted";
		case SocketError::TimedOut: return "timed out";
		case SocketError::AddressInUse: return "address in use";
		case SocketError::Unreachable: return "unreachable";
		case SocketError::Other: return "socket error";
		}
		return "socket error";
	}

	std::string DescribeLastSocketError(SocketError error)
	{
		return std::format("{} ({})", ToString(error), LastNativeSocketError());
	}

}
