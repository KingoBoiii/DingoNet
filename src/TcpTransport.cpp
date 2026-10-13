#include "DingoNet/TcpTransport.h"

#include "Internal/Log.h"
#include "Internal/SendLimits.h"
#include "Platform/Socket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>

namespace Dingo::Net
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		constexpr size_t FRAME_HEADER_BYTES = 4;
		constexpr double CLOSE_FLUSH_SECONDS = 0.25;

		Clock::duration Seconds(double seconds)
		{
			// Casting a huge or infinite double to the clock's integer ticks is undefined.
			const double clamped = std::isnan(seconds) ? 0.0 : std::clamp(seconds, 0.0, 1e6);
			return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(clamped));
		}

		enum class ReadStatus
		{
			Open,
			Finished,
			Failed
		};

		// Reads what has arrived, up to budget bytes; a null sink drops it. Finished: the other side's FIN.
		ReadStatus ReadAvailable(Internal::SocketHandle socket, std::vector<uint8_t>* sink, size_t budget, uint64_t& bytesIn, Internal::SocketError& error)
		{
			std::array<uint8_t, 16384> buffer;
			size_t taken = 0;
			budget = (std::max)(budget, static_cast<size_t>(1));
			while (taken < budget)
			{
				const size_t want = (std::min)(buffer.size(), budget - taken);
				const ptrdiff_t got = Internal::Receive(socket, buffer.data(), want, error);
				if (got > 0)
				{
					taken += static_cast<size_t>(got);
					if (sink)
					{
						sink->insert(sink->end(), buffer.data(), buffer.data() + got);
						bytesIn += static_cast<uint64_t>(got);
					}
					continue;
				}
				if (got == 0)
				{
					return ReadStatus::Finished;
				}
				return error == Internal::SocketError::WouldBlock ? ReadStatus::Open : ReadStatus::Failed;
			}
			return ReadStatus::Open;
		}

		DisconnectReason ReasonFor(Internal::SocketError error)
		{
			switch (error)
			{
			case Internal::SocketError::Reset:
			case Internal::SocketError::Aborted: return DisconnectReason::Reset;
			case Internal::SocketError::TimedOut: return DisconnectReason::TimedOut;
			default: return DisconnectReason::Error;
			}
		}

	}

	struct TcpTransport::Connection
	{
		Internal::SocketHandle Socket = Internal::INVALID_SOCKET_HANDLE;
		PeerId Id = 0;
		std::vector<uint8_t> In;
		std::vector<uint8_t> Out;
		bool Connecting = false;
		Clock::time_point ConnectStarted{};
		bool Closed = false;
		DisconnectReason Reason = DisconnectReason::Closed;
		std::string Detail;
		// We hung up: what is queued still goes out, then our side shuts, and we read (and drop) until
		// theirs is too, so the close is a clean FIN and a parting message isn't lost to a reset.
		bool Closing = false;
		bool SendShut = false;
		bool PeerFinished = false;
		Clock::time_point CloseBy{};
	};

	TcpTransport::TcpTransport(const TcpParams& params)
		: m_Params(params), m_Listen(Internal::INVALID_SOCKET_HANDLE)
	{
	}

	std::unique_ptr<TcpTransport> TcpTransport::Listen(const std::string& address, uint16_t port, std::string& error, const TcpParams& params)
	{
		std::unique_ptr<TcpTransport> t(new TcpTransport(params));
		if (!Internal::AcquireSocketLibrary(error))
		{
			return nullptr;
		}
		t->m_LibraryAcquired = true;

		Internal::SocketAddress bindAddress;
		if (!Internal::ParseAddress(address, port, bindAddress))
		{
			error = std::format("'{}' is not an IPv4 address", address);
			return nullptr;
		}

		Internal::SocketError socketError;
		const Internal::SocketHandle s = Internal::OpenStreamSocket(bindAddress.Family, socketError);
		if (s == Internal::INVALID_SOCKET_HANDLE)
		{
			error = std::format("no socket: {}", Internal::DescribeLastSocketError(socketError));
			return nullptr;
		}
		t->m_Listen = s;
		t->m_Listening = true;

		if ((socketError = Internal::ConfigureListener(s)) != Internal::SocketError::None
			|| (socketError = Internal::Bind(s, bindAddress)) != Internal::SocketError::None
			|| (socketError = Internal::Listen(s, 8)) != Internal::SocketError::None
			|| (socketError = Internal::ConfigureStream(s)) != Internal::SocketError::None)
		{
			error = std::format("can't listen on {}:{}: {}", address, port, Internal::DescribeLastSocketError(socketError));
			return nullptr;
		}

		Internal::SocketAddress bound;
		if (Internal::GetLocalAddress(s, bound) == Internal::SocketError::None)
		{
			t->m_Port = bound.Port;
		}
		t->m_Describe = std::format("tcp host {}:{}", address, t->m_Port);
		return t;
	}

	std::unique_ptr<TcpTransport> TcpTransport::Connect(const std::string& address, uint16_t port, std::string& error, const TcpParams& params)
	{
		std::unique_ptr<TcpTransport> t(new TcpTransport(params));
		if (!Internal::AcquireSocketLibrary(error))
		{
			return nullptr;
		}
		t->m_LibraryAcquired = true;

		Internal::SocketAddress remote;
		if (!Internal::ParseAddress(address, port, remote))
		{
			error = std::format("'{}' is not an IPv4 address", address);
			return nullptr;
		}

		Internal::SocketError socketError;
		const Internal::SocketHandle s = Internal::OpenStreamSocket(remote.Family, socketError);
		if (s == Internal::INVALID_SOCKET_HANDLE)
		{
			error = std::format("no socket: {}", Internal::DescribeLastSocketError(socketError));
			return nullptr;
		}

		auto c = std::make_unique<Connection>();
		c->Socket = s;
		c->Id = t->m_NextId++;
		c->Connecting = true;
		c->ConnectStarted = Clock::now();
		t->m_Port = port;
		t->m_Describe = std::format("tcp client -> {}:{}", address, port);

		// A connect that fails at once still reports through Poll, like one that fails later.
		socketError = Internal::ConfigureStream(s);
		if (socketError == Internal::SocketError::None)
		{
			socketError = Internal::Connect(s, remote);
		}
		if (socketError != Internal::SocketError::None && socketError != Internal::SocketError::InProgress)
		{
			const DisconnectReason reason = socketError == Internal::SocketError::Refused ? DisconnectReason::Refused : DisconnectReason::ConnectFailed;
			t->CloseConnection(*c, reason, std::format("can't connect: {}", Internal::DescribeLastSocketError(socketError)));
		}
		t->m_Connections.push_back(std::move(c));
		return t;
	}

	TcpTransport::~TcpTransport()
	{
		Close();
	}

	void TcpTransport::Send(PeerId to, std::span<const uint8_t> data, Delivery delivery)
	{
		if (!Internal::CheckUnreliableSize(data.size(), delivery, "tcp"))
		{
			return;
		}
		if (data.size() > m_Params.MaxFrameBytes)
		{
			Internal::LogError("tcp: dropped a packet of {} bytes (the frame limit is {})", data.size(), m_Params.MaxFrameBytes);
			return;
		}

		for (auto& owned : m_Connections)
		{
			Connection& c = *owned;
			if (c.Id != to || c.Closed || c.Connecting || c.Closing)
			{
				continue;
			}
			const uint32_t size = static_cast<uint32_t>(data.size());
			for (size_t i = 0; i < FRAME_HEADER_BYTES; i++)
			{
				c.Out.push_back(static_cast<uint8_t>(size >> (8 * i)));
			}
			c.Out.insert(c.Out.end(), data.begin(), data.end());
			BytesOut += FRAME_HEADER_BYTES + data.size();
			return;
		}
	}

	void TcpTransport::CloseConnection(Connection& c, DisconnectReason reason, std::string detail)
	{
		if (c.Closed)
		{
			return;
		}
		Internal::CloseSocket(c.Socket);
		c.Closed = true;
		c.Reason = reason;
		c.Detail = std::move(detail);
	}

	bool TcpTransport::Flush(Connection& c)
	{
		size_t sent = 0;
		while (sent < c.Out.size())
		{
			Internal::SocketError error;
			const ptrdiff_t moved = Internal::Send(c.Socket, c.Out.data() + sent, c.Out.size() - sent, error);
			if (moved > 0)
			{
				sent += static_cast<size_t>(moved);
				continue;
			}
			if (moved < 0 && error != Internal::SocketError::WouldBlock)
			{
				CloseConnection(c, ReasonFor(error), std::format("connection lost: {}", Internal::DescribeLastSocketError(error)));
				return false;
			}
			break;
		}
		c.Out.erase(c.Out.begin(), c.Out.begin() + static_cast<ptrdiff_t>(sent));
		return true;
	}

	void TcpTransport::AcceptPending(std::vector<PeerEvent>& events)
	{
		while (m_Listening)
		{
			Internal::SocketError error;
			const Internal::SocketHandle s = Internal::Accept(m_Listen, error);
			if (s == Internal::INVALID_SOCKET_HANDLE)
			{
				if (error != Internal::SocketError::WouldBlock && error != Internal::SocketError::Aborted)
				{
					Internal::LogWarn("tcp: accept failed: {}", Internal::DescribeLastSocketError(error));
				}
				break;
			}
			const auto live = std::count_if(m_Connections.begin(), m_Connections.end(),
				[](const std::unique_ptr<Connection>& c) { return !c->Closed && !c->Closing; });
			if (static_cast<size_t>(live) >= m_Params.MaxConnections)
			{
				Internal::LogWarn("tcp: turned a connection away ({} is the limit)", m_Params.MaxConnections);
				Internal::CloseSocket(s);
				continue;
			}
			if ((error = Internal::ConfigureStream(s)) != Internal::SocketError::None)
			{
				Internal::LogWarn("tcp: dropped a new connection: {}", Internal::DescribeLastSocketError(error));
				Internal::CloseSocket(s);
				continue;
			}
			auto c = std::make_unique<Connection>();
			c->Socket = s;
			c->Id = m_NextId++;
			events.push_back({ PeerEvent::Kind::Connected, c->Id, DisconnectReason::Closed, {} });
			m_Connections.push_back(std::move(c));
		}
	}

	void TcpTransport::PollClosing(Connection& c, Clock::time_point now)
	{
		if (!c.PeerFinished)
		{
			Internal::SocketError error;
			uint64_t discarded = 0;
			const ReadStatus status = ReadAvailable(c.Socket, nullptr, m_Params.MaxReadBytesPerPoll, discarded, error);
			if (status == ReadStatus::Failed)
			{
				CloseConnection(c, ReasonFor(error), "lost while closing");
				return;
			}
			c.PeerFinished = status == ReadStatus::Finished;
		}
		if (!c.SendShut)
		{
			if (!Flush(c))
			{
				return;
			}
			if (c.Out.empty())
			{
				Internal::ShutdownSend(c.Socket);
				c.SendShut = true;
			}
		}
		if ((c.SendShut && c.PeerFinished) || now > c.CloseBy)
		{
			Internal::CloseSocket(c.Socket);
			c.Closed = true;
		}
	}

	void TcpTransport::Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events)
	{
		const Clock::time_point now = Clock::now();
		AcceptPending(events);

		for (auto& owned : m_Connections)
		{
			Connection& c = *owned;
			if (c.Closed)
			{
				continue;
			}

			if (c.Closing)
			{
				PollClosing(c, now);
				continue;
			}

			if (c.Connecting)
			{
				Internal::SocketError error;
				const Internal::ConnectStatus status = Internal::PollConnect(c.Socket, error);
				if (status == Internal::ConnectStatus::Connected)
				{
					c.Connecting = false;
					events.push_back({ PeerEvent::Kind::Connected, c.Id, DisconnectReason::Closed, {} });
				}
				else if (status == Internal::ConnectStatus::Failed)
				{
					const DisconnectReason reason = error == Internal::SocketError::Refused ? DisconnectReason::Refused : DisconnectReason::ConnectFailed;
					CloseConnection(c, reason, std::format("can't connect: {}", Internal::DescribeLastSocketError(error)));
				}
				else if (now - c.ConnectStarted > Seconds(m_Params.ConnectTimeout))
				{
					CloseConnection(c, DisconnectReason::ConnectFailed, std::format("connect timed out after {} s", m_Params.ConnectTimeout));
				}
				continue;
			}

			// Read before any send: a send to a peer that has already closed brings back a reset, and
			// Windows then throws away what we hadn't read yet, a parting Bye with it.
			Internal::SocketError error;
			const ReadStatus status = ReadAvailable(c.Socket, &c.In, m_Params.MaxReadBytesPerPoll, BytesIn, error);
			if (status == ReadStatus::Finished)
			{
				CloseConnection(c, DisconnectReason::Closed, "closed by the other end");
			}
			else if (status == ReadStatus::Failed)
			{
				CloseConnection(c, ReasonFor(error), std::format("connection lost: {}", Internal::DescribeLastSocketError(error)));
			}

			size_t read = 0;
			while (c.In.size() - read >= FRAME_HEADER_BYTES)
			{
				const uint8_t* header = c.In.data() + read;
				const uint32_t size = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<uint32_t>(header[3]) << 24);
				if (size > m_Params.MaxFrameBytes)
				{
					Internal::LogWarn("tcp: peer {} sent a frame of {} bytes (the limit is {})", c.Id, size, m_Params.MaxFrameBytes);
					CloseConnection(c, DisconnectReason::Error, std::format("bad frame of {} bytes", size));
					break;
				}
				if (c.In.size() - read - FRAME_HEADER_BYTES < size)
				{
					break;
				}
				const auto begin = c.In.begin() + static_cast<ptrdiff_t>(read + FRAME_HEADER_BYTES);
				packets.push_back({ c.Id, std::vector<uint8_t>(begin, begin + size) });
				read += FRAME_HEADER_BYTES + size;
			}
			c.In.erase(c.In.begin(), c.In.begin() + static_cast<ptrdiff_t>(read));
			if (c.Closed || !Flush(c))
			{
				continue;
			}
			if (c.Out.size() > m_Params.MaxQueuedBytes)
			{
				Internal::LogWarn("tcp: dropped peer {}: {} bytes queued and it isn't reading", c.Id, c.Out.size());
				CloseConnection(c, DisconnectReason::Error, "not reading");
			}
		}

		for (auto it = m_Connections.begin(); it != m_Connections.end();)
		{
			Connection& c = **it;
			if (!c.Closed)
			{
				++it;
				continue;
			}
			// A hang-up we asked for (Closing) was never theirs to hear about.
			if (!c.Closing)
			{
				events.push_back({ PeerEvent::Kind::Disconnected, c.Id, c.Reason, std::move(c.Detail) });
			}
			it = m_Connections.erase(it);
		}
	}

	void TcpTransport::Disconnect(PeerId peer)
	{
		for (auto& owned : m_Connections)
		{
			Connection& c = *owned;
			if (c.Id != peer || c.Closed || c.Closing)
			{
				continue;
			}
			c.Closing = true;
			if (c.Connecting)
			{
				Internal::CloseSocket(c.Socket);
				c.Closed = true;
				continue;
			}
			c.CloseBy = Clock::now() + Seconds(m_Params.CloseLinger);
		}
	}

	void TcpTransport::Close()
	{
		// Leaving for good: a short flush of what's queued, a clean hang-up, and a drain of what's
		// already here, so the other side reads our parting message instead of a reset.
		const Clock::time_point flushBy = Clock::now() + Seconds(CLOSE_FLUSH_SECONDS);
		for (auto& owned : m_Connections)
		{
			Connection& c = *owned;
			if (c.Closed)
			{
				continue;
			}
			if (!c.Connecting && !c.SendShut)
			{
				while (Flush(c) && !c.Out.empty())
				{
					const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(flushBy - Clock::now()).count();
					if (left <= 0)
					{
						break;
					}
					Internal::WaitWritable(c.Socket, static_cast<int>(left));
				}
				if (c.Closed)
				{
					continue;
				}
				Internal::ShutdownSend(c.Socket);
			}
			Internal::SocketError ignored;
			uint64_t discarded = 0;
			ReadAvailable(c.Socket, nullptr, m_Params.MaxReadBytesPerPoll, discarded, ignored);
			Internal::CloseSocket(c.Socket);
		}
		m_Connections.clear();
		if (m_Listening)
		{
			Internal::CloseSocket(m_Listen);
		}
		m_Listening = false;
		if (m_LibraryAcquired)
		{
			Internal::ReleaseSocketLibrary();
		}
		m_LibraryAcquired = false;
	}

}
