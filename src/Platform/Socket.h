#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Dingo::Net::Internal
{

	using SocketHandle = uint64_t;
	inline constexpr SocketHandle INVALID_SOCKET_HANDLE = ~SocketHandle(0);

	enum class SocketError : uint8_t
	{
		None,
		WouldBlock,
		InProgress,
		Refused,
		Reset,
		Aborted,
		TimedOut,
		AddressInUse,
		Unreachable,
		Other
	};

	enum class AddressFamily : uint8_t
	{
		IPv4
	};

	struct SocketAddress
	{
		AddressFamily Family = AddressFamily::IPv4;
		std::array<uint8_t, 16> Bytes{};
		uint16_t Port = 0;
	};

	enum class ConnectStatus : uint8_t
	{
		Pending,
		Connected,
		Failed
	};

	// Refcounted and thread-safe: Winsock starts with the first holder and stops with the last.
	bool AcquireSocketLibrary(std::string& error);
	void ReleaseSocketLibrary();

	bool ParseAddress(std::string_view text, uint16_t port, SocketAddress& out);
	std::string FormatAddress(const SocketAddress& address);

	// Sockets (accepted ones too) are never inherited by a child process.
	SocketHandle OpenStreamSocket(AddressFamily family, SocketError& error);
	// For a socket something else created (ENet's): no child process inherits it from now on.
	SocketError MakeNonInheritable(SocketHandle socket);
	// Non-blocking, no Nagle, and (where the OS has it) no SIGPIPE.
	SocketError ConfigureStream(SocketHandle socket);
	// A second host on a bound port fails instead of sharing it; a restart can rebind past TIME_WAIT.
	SocketError ConfigureListener(SocketHandle socket);
	SocketError Bind(SocketHandle socket, const SocketAddress& address);
	SocketError Listen(SocketHandle socket, int backlog);
	SocketError GetLocalAddress(SocketHandle socket, SocketAddress& out);
	SocketHandle Accept(SocketHandle listener, SocketError& error);
	// None or InProgress on success; the outcome of InProgress comes from PollConnect.
	SocketError Connect(SocketHandle socket, const SocketAddress& address);
	ConnectStatus PollConnect(SocketHandle socket, SocketError& error);
	// Blocks up to timeoutMs until the socket can take more bytes; only for a final flush.
	void WaitWritable(SocketHandle socket, int timeoutMs);

	// Bytes moved; 0 from Receive is an orderly close; -1 sets error (WouldBlock when nothing moved).
	ptrdiff_t Send(SocketHandle socket, const uint8_t* data, size_t size, SocketError& error);
	ptrdiff_t Receive(SocketHandle socket, uint8_t* buffer, size_t size, SocketError& error);

	void ShutdownSend(SocketHandle socket);
	void CloseSocket(SocketHandle socket);
	// Closes with a reset instead of a FIN.
	void AbortSocket(SocketHandle socket);

	// The OS error code behind the last failure on this thread.
	int LastNativeSocketError();
	const char* ToString(SocketError error);
	std::string DescribeLastSocketError(SocketError error);

}
