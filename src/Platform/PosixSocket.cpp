#include "Platform/Socket.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

// Without it a send to a closed peer raises SIGPIPE, whose default action kills the game.
#ifdef MSG_NOSIGNAL
	#define DN_SEND_FLAGS MSG_NOSIGNAL
#else
	#define DN_SEND_FLAGS 0
#endif

namespace Dingo::Net::Internal
{

	namespace
	{

		thread_local int s_LastError = 0;

		int Native(SocketHandle socket)
		{
			return static_cast<int>(socket);
		}

		SocketError Map(int code)
		{
			s_LastError = code;
			switch (code)
			{
			case 0: return SocketError::None;
			case EAGAIN:
#if EWOULDBLOCK != EAGAIN
			case EWOULDBLOCK:
#endif
				return SocketError::WouldBlock;
			case EINPROGRESS:
			case EALREADY: return SocketError::InProgress;
			case ECONNREFUSED: return SocketError::Refused;
			case ECONNRESET:
			case EPIPE:
			case ENETRESET: return SocketError::Reset;
			case ECONNABORTED: return SocketError::Aborted;
			case ETIMEDOUT: return SocketError::TimedOut;
			case EADDRINUSE: return SocketError::AddressInUse;
			case ENETUNREACH:
			case EHOSTUNREACH:
			case ENETDOWN: return SocketError::Unreachable;
			default: return SocketError::Other;
			}
		}

		SocketError LastError()
		{
			return Map(errno);
		}

		bool SetCloseOnExec(int fd)
		{
			const int flags = fcntl(fd, F_GETFD, 0);
			return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
		}

		void ToNative(const SocketAddress& address, sockaddr_in& out)
		{
			out = {};
			out.sin_family = AF_INET;
			out.sin_port = htons(address.Port);
			std::memcpy(&out.sin_addr, address.Bytes.data(), 4);
		}

	}

	bool AcquireSocketLibrary(std::string&)
	{
		return true;
	}

	void ReleaseSocketLibrary()
	{
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
#ifdef SOCK_CLOEXEC
		const int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
		const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
		if (s < 0)
		{
			error = LastError();
			return INVALID_SOCKET_HANDLE;
		}
#ifndef SOCK_CLOEXEC
		SetCloseOnExec(s);
#endif
		error = SocketError::None;
		return static_cast<SocketHandle>(s);
	}

	SocketError MakeNonInheritable(SocketHandle socket)
	{
		return SetCloseOnExec(static_cast<int>(socket)) ? SocketError::None : LastError();
	}

	SocketError DisableUdpConnReset(SocketHandle)
	{
		return SocketError::None;
	}

	SocketError ConfigureStream(SocketHandle socket)
	{
		const int flags = fcntl(Native(socket), F_GETFL, 0);
		if (flags < 0 || fcntl(Native(socket), F_SETFL, flags | O_NONBLOCK) < 0)
		{
			return LastError();
		}
		int noDelay = 1;
		setsockopt(Native(socket), IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
#ifdef SO_NOSIGPIPE
		int noSigPipe = 1;
		setsockopt(Native(socket), SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
		return SocketError::None;
	}

	SocketError ConfigureListener(SocketHandle socket)
	{
		int reuse = 1;
		if (setsockopt(Native(socket), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
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
		socklen_t length = sizeof(native);
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
		int s;
		do
		{
#if defined(__linux__) && defined(SOCK_CLOEXEC)
			s = accept4(Native(listener), nullptr, nullptr, SOCK_CLOEXEC);
#else
			s = accept(Native(listener), nullptr, nullptr);
#endif
		} while (s < 0 && errno == EINTR);
		if (s < 0)
		{
			error = LastError();
			return INVALID_SOCKET_HANDLE;
		}
#if !defined(__linux__) || !defined(SOCK_CLOEXEC)
		SetCloseOnExec(s);
#endif
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
		// An interrupted connect carries on asynchronously; it can't be retried.
		if (errno == EINTR)
		{
			s_LastError = EINTR;
			return SocketError::InProgress;
		}
		const SocketError error = LastError();
		return error == SocketError::WouldBlock ? SocketError::InProgress : error;
	}

	ConnectStatus PollConnect(SocketHandle socket, SocketError& error)
	{
		pollfd entry{};
		entry.fd = Native(socket);
		entry.events = POLLOUT;
		int ready;
		do
		{
			ready = poll(&entry, 1, 0);
		} while (ready < 0 && errno == EINTR);
		if (ready < 0)
		{
			error = LastError();
			return ConnectStatus::Failed;
		}
		if (ready == 0)
		{
			error = SocketError::None;
			return ConnectStatus::Pending;
		}

		int code = 0;
		socklen_t length = sizeof(code);
		if (getsockopt(Native(socket), SOL_SOCKET, SO_ERROR, &code, &length) != 0)
		{
			error = LastError();
			return ConnectStatus::Failed;
		}
		if (code != 0 || (entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
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
		pollfd entry{};
		entry.fd = Native(socket);
		entry.events = POLLOUT;
		poll(&entry, 1, timeoutMs);
	}

	ptrdiff_t Send(SocketHandle socket, const uint8_t* data, size_t size, SocketError& error)
	{
		ssize_t sent;
		do
		{
			sent = send(Native(socket), data, size, DN_SEND_FLAGS);
		} while (sent < 0 && errno == EINTR);
		if (sent < 0)
		{
			error = LastError();
			return -1;
		}
		error = SocketError::None;
		return static_cast<ptrdiff_t>(sent);
	}

	ptrdiff_t Receive(SocketHandle socket, uint8_t* buffer, size_t size, SocketError& error)
	{
		ssize_t got;
		do
		{
			got = recv(Native(socket), buffer, size, 0);
		} while (got < 0 && errno == EINTR);
		if (got < 0)
		{
			error = LastError();
			return -1;
		}
		error = SocketError::None;
		return static_cast<ptrdiff_t>(got);
	}

	SocketHandle OpenUdpSocket(AddressFamily, SocketError& error)
	{
#ifdef SOCK_CLOEXEC
		const int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
#else
		const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
		if (s < 0)
		{
			error = LastError();
			return INVALID_SOCKET_HANDLE;
		}
#ifndef SOCK_CLOEXEC
		SetCloseOnExec(s);
#endif
		const int flags = fcntl(s, F_GETFL, 0);
		if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0)
		{
			error = LastError();
			close(s);
			return INVALID_SOCKET_HANDLE;
		}
		error = SocketError::None;
		return static_cast<SocketHandle>(s);
	}

	SocketError SetBroadcast(SocketHandle socket, bool enabled)
	{
		int value = enabled ? 1 : 0;
		if (setsockopt(Native(socket), SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) != 0)
		{
			return LastError();
		}
		return SocketError::None;
	}

	void ListBroadcastAddresses(std::vector<SocketAddress>& out, uint16_t port)
	{
		ifaddrs* list = nullptr;
		if (getifaddrs(&list) != 0)
		{
			return;
		}
		for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
		{
			if (entry->ifa_addr == nullptr || entry->ifa_broadaddr == nullptr || entry->ifa_addr->sa_family != AF_INET)
			{
				continue;
			}
			if ((entry->ifa_flags & (IFF_UP | IFF_BROADCAST)) != (IFF_UP | IFF_BROADCAST) || (entry->ifa_flags & IFF_LOOPBACK))
			{
				continue;
			}
			const uint32_t directed = reinterpret_cast<const sockaddr_in*>(entry->ifa_broadaddr)->sin_addr.s_addr;
			if (directed == 0xFFFFFFFFu)
			{
				continue;
			}
			SocketAddress to;
			to.Port = port;
			std::memcpy(to.Bytes.data(), &directed, sizeof(directed));
			out.push_back(to);
		}
		freeifaddrs(list);
	}

	ptrdiff_t SendTo(SocketHandle socket, const uint8_t* data, size_t size, const SocketAddress& to, SocketError& error)
	{
		sockaddr_in native;
		ToNative(to, native);
		ssize_t sent;
		do
		{
			sent = sendto(Native(socket), data, size, DN_SEND_FLAGS, reinterpret_cast<const sockaddr*>(&native), sizeof(native));
		} while (sent < 0 && errno == EINTR);
		if (sent < 0)
		{
			error = LastError();
			return -1;
		}
		error = SocketError::None;
		return static_cast<ptrdiff_t>(sent);
	}

	ptrdiff_t RecvFrom(SocketHandle socket, uint8_t* buffer, size_t size, SocketAddress& from, SocketError& error)
	{
		sockaddr_in native{};
		socklen_t length = sizeof(native);
		ssize_t got;
		do
		{
			length = sizeof(native);
			got = recvfrom(Native(socket), buffer, size, 0, reinterpret_cast<sockaddr*>(&native), &length);
		} while (got < 0 && errno == EINTR);
		if (got < 0)
		{
			error = LastError();
			return -1;
		}
		from = {};
		from.Family = AddressFamily::IPv4;
		from.Port = ntohs(native.sin_port);
		std::memcpy(from.Bytes.data(), &native.sin_addr, 4);
		error = SocketError::None;
		return static_cast<ptrdiff_t>(got);
	}

	void ShutdownSend(SocketHandle socket)
	{
		shutdown(Native(socket), SHUT_WR);
	}

	void CloseSocket(SocketHandle socket)
	{
		close(Native(socket));
	}

	void AbortSocket(SocketHandle socket)
	{
		linger hard{};
		hard.l_onoff = 1;
		hard.l_linger = 0;
		setsockopt(Native(socket), SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
		close(Native(socket));
	}

	int LastNativeSocketError()
	{
		return s_LastError;
	}

	int CurrentNativeSocketError()
	{
		return errno;
	}

}
