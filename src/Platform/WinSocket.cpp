#include "Platform/Socket.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <climits>
#include <cstring>
#include <mutex>

// Consumers of the static library link Winsock without naming it.
#pragma comment(lib, "ws2_32.lib")

namespace Dingo::Net::Internal
{

	namespace
	{

		std::mutex s_LibraryMutex;
		int s_LibraryUsers = 0;
		thread_local int s_LastError = 0;

		SOCKET Native(SocketHandle socket)
		{
			return static_cast<SOCKET>(socket);
		}

		SocketError Map(int code)
		{
			s_LastError = code;
			switch (code)
			{
			case 0: return SocketError::None;
			case WSAEWOULDBLOCK:
			case WSAEINPROGRESS: return SocketError::WouldBlock;
			case WSAECONNREFUSED: return SocketError::Refused;
			case WSAECONNRESET:
			case WSAENETRESET: return SocketError::Reset;
			case WSAECONNABORTED: return SocketError::Aborted;
			case WSAETIMEDOUT: return SocketError::TimedOut;
			case WSAEADDRINUSE: return SocketError::AddressInUse;
			case WSAENETUNREACH:
			case WSAEHOSTUNREACH:
			case WSAENETDOWN: return SocketError::Unreachable;
			default: return SocketError::Other;
			}
		}

		SocketError LastError()
		{
			return Map(WSAGetLastError());
		}

		void ToNative(const SocketAddress& address, sockaddr_in& out)
		{
			out = {};
			out.sin_family = AF_INET;
			out.sin_port = htons(address.Port);
			std::memcpy(&out.sin_addr, address.Bytes.data(), 4);
		}

	}

	bool AcquireSocketLibrary(std::string& error)
	{
		std::lock_guard lock(s_LibraryMutex);
		if (s_LibraryUsers == 0)
		{
			WSADATA data;
			const int result = WSAStartup(MAKEWORD(2, 2), &data);
			if (result != 0)
			{
				s_LastError = result;
				error = "Winsock would not start";
				return false;
			}
		}
		s_LibraryUsers++;
		return true;
	}

	void ReleaseSocketLibrary()
	{
		std::lock_guard lock(s_LibraryMutex);
		if (s_LibraryUsers > 0 && --s_LibraryUsers == 0)
		{
			WSACleanup();
		}
	}

	bool ParseAddress(std::string_view text, uint16_t port, SocketAddress& out)
	{
		out = {};
		out.Family = AddressFamily::IPv4;
		out.Port = port;
		const std::string terminated(text);
		in_addr parsed{};
		if (inet_pton(AF_INET, terminated.c_str(), &parsed) != 1)
		{
			return false;
		}
		std::memcpy(out.Bytes.data(), &parsed, 4);
		return true;
	}

	std::string FormatAddress(const SocketAddress& address)
	{
		char text[INET_ADDRSTRLEN] = {};
		in_addr raw{};
		std::memcpy(&raw, address.Bytes.data(), 4);
		inet_ntop(AF_INET, &raw, text, sizeof(text));
		return std::string(text) + ":" + std::to_string(address.Port);
	}

	SocketHandle OpenStreamSocket(AddressFamily, SocketError& error)
	{
		const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
		if (s == INVALID_SOCKET)
		{
			error = LastError();
			return INVALID_SOCKET_HANDLE;
		}
		error = SocketError::None;
		return static_cast<SocketHandle>(s);
	}

	SocketError ConfigureStream(SocketHandle socket)
	{
		u_long nonBlocking = 1;
		if (ioctlsocket(Native(socket), FIONBIO, &nonBlocking) != 0)
		{
			return LastError();
		}
		BOOL noDelay = TRUE;
		setsockopt(Native(socket), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
		return SocketError::None;
	}

	SocketError ConfigureListener(SocketHandle socket)
	{
		BOOL exclusive = TRUE;
		if (setsockopt(Native(socket), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
		{
			return LastError();
		}
		return SocketError::None;
	}

	SocketError Bind(SocketHandle socket, const SocketAddress& address)
	{
		sockaddr_in native;
		ToNative(address, native);
		if (bind(Native(socket), reinterpret_cast<const sockaddr*>(&native), sizeof(native)) != 0)
		{
			return LastError();
		}
		return SocketError::None;
	}

	SocketError Listen(SocketHandle socket, int backlog)
	{
		if (listen(Native(socket), backlog) != 0)
		{
			return LastError();
		}
		return SocketError::None;
	}

	SocketError GetLocalAddress(SocketHandle socket, SocketAddress& out)
	{
		sockaddr_in native{};
		int length = sizeof(native);
		if (getsockname(Native(socket), reinterpret_cast<sockaddr*>(&native), &length) != 0)
		{
			return LastError();
		}
		out = {};
		out.Family = AddressFamily::IPv4;
		out.Port = ntohs(native.sin_port);
		std::memcpy(out.Bytes.data(), &native.sin_addr, 4);
		return SocketError::None;
	}

	SocketHandle Accept(SocketHandle listener, SocketError& error)
	{
		const SOCKET s = accept(Native(listener), nullptr, nullptr);
		if (s == INVALID_SOCKET)
		{
			error = LastError();
			return INVALID_SOCKET_HANDLE;
		}
		SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
		error = SocketError::None;
		return static_cast<SocketHandle>(s);
	}

	SocketError Connect(SocketHandle socket, const SocketAddress& address)
	{
		sockaddr_in native;
		ToNative(address, native);
		if (connect(Native(socket), reinterpret_cast<const sockaddr*>(&native), sizeof(native)) == 0)
		{
			return SocketError::None;
		}
		const SocketError error = LastError();
		return error == SocketError::WouldBlock ? SocketError::InProgress : error;
	}

	ConnectStatus PollConnect(SocketHandle socket, SocketError& error)
	{
		// Winsock reports a failed non-blocking connect in the except set, not as writable.
		fd_set writable;
		fd_set failed;
		FD_ZERO(&writable);
		FD_ZERO(&failed);
		FD_SET(Native(socket), &writable);
		FD_SET(Native(socket), &failed);
		timeval zero{ 0, 0 };
		if (select(0, nullptr, &writable, &failed, &zero) == SOCKET_ERROR)
		{
			error = LastError();
			return ConnectStatus::Failed;
		}
		if (!FD_ISSET(Native(socket), &writable) && !FD_ISSET(Native(socket), &failed))
		{
			error = SocketError::None;
			return ConnectStatus::Pending;
		}

		int code = 0;
		int length = sizeof(code);
		if (getsockopt(Native(socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0)
		{
			error = LastError();
			return ConnectStatus::Failed;
		}
		if (code != 0 || FD_ISSET(Native(socket), &failed))
		{
			s_LastError = code;
			error = code != 0 ? Map(code) : SocketError::Other;
			return ConnectStatus::Failed;
		}
		error = SocketError::None;
		return ConnectStatus::Connected;
	}

	void WaitWritable(SocketHandle socket, int timeoutMs)
	{
		fd_set writable;
		FD_ZERO(&writable);
		FD_SET(Native(socket), &writable);
		timeval wait{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
		select(0, nullptr, &writable, nullptr, &wait);
	}

	ptrdiff_t Send(SocketHandle socket, const uint8_t* data, size_t size, SocketError& error)
	{
		const int length = static_cast<int>(size < static_cast<size_t>(INT_MAX) ? size : static_cast<size_t>(INT_MAX));
		const int sent = send(Native(socket), reinterpret_cast<const char*>(data), length, 0);
		if (sent == SOCKET_ERROR)
		{
			error = LastError();
			return -1;
		}
		error = SocketError::None;
		return sent;
	}

	ptrdiff_t Receive(SocketHandle socket, uint8_t* buffer, size_t size, SocketError& error)
	{
		const int length = static_cast<int>(size < static_cast<size_t>(INT_MAX) ? size : static_cast<size_t>(INT_MAX));
		const int got = recv(Native(socket), reinterpret_cast<char*>(buffer), length, 0);
		if (got == SOCKET_ERROR)
		{
			error = LastError();
			return -1;
		}
		error = SocketError::None;
		return got;
	}

	void ShutdownSend(SocketHandle socket)
	{
		shutdown(Native(socket), SD_SEND);
	}

	void CloseSocket(SocketHandle socket)
	{
		closesocket(Native(socket));
	}

	void AbortSocket(SocketHandle socket)
	{
		linger hard{ 1, 0 };
		setsockopt(Native(socket), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&hard), sizeof(hard));
		closesocket(Native(socket));
	}

	int LastNativeSocketError()
	{
		return s_LastError;
	}

}
