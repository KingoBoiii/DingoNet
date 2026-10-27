#include "DingoNet/LanDiscovery.h"

#include "DingoNet/Bytes.h"
#include "DingoNet/Session.h"
#include "Internal/Log.h"
#include "Platform/Socket.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Dingo::Net
{

	namespace
	{

		using namespace Internal;

		constexpr std::array<uint8_t, 5> MAGIC = { 'D', 'N', 'L', 'A', 'N' };
		constexpr uint8_t DISCOVERY_VERSION = 1;
		constexpr uint8_t TYPE_QUERY = 1;
		constexpr uint8_t TYPE_REPLY = 2;
		// A query is padded to this size, and a host answers only a query at least as large as the reply it
		// would send, so a spoofed source never receives more than it sent.
		constexpr size_t QUERY_SIZE = 512;
		constexpr size_t MAX_NAME_BYTES = 64;
		constexpr size_t RECEIVE_BUFFER = 1500;
		constexpr int MAX_DATAGRAMS_PER_POLL = 256;
		constexpr int MAX_ANSWERS_PER_POLL = 32;
		constexpr int MAX_SEATS = 32;

		void WriteHeader(ByteWriter& out, uint8_t type)
		{
			for (const uint8_t b : MAGIC)
			{
				out.U8(b);
			}
			out.U8(type);
			out.U8(DISCOVERY_VERSION);
		}

		bool ReadHeader(ByteReader& in, uint8_t expectedType)
		{
			for (const uint8_t b : MAGIC)
			{
				if (in.U8() != b)
				{
					return false;
				}
			}
			const uint8_t type = in.U8();
			const uint8_t version = in.U8();
			return in.Ok() && type == expectedType && version == DISCOVERY_VERSION;
		}

		std::string FilterName(const std::function<std::string(std::string_view)>& filter, std::string_view name)
		{
			std::string filtered = filter ? filter(name) : DefaultNameFilter(name);
			if (filtered.size() > MAX_NAME_BYTES)
			{
				filtered.resize(MAX_NAME_BYTES);
			}
			return filtered;
		}

		std::vector<uint8_t> EncodeQuery(uint16_t protocol)
		{
			ByteWriter out;
			WriteHeader(out, TYPE_QUERY);
			out.U16(protocol);
			std::vector<uint8_t> bytes = out.Data();
			bytes.resize(QUERY_SIZE, 0);
			return bytes;
		}

		std::vector<uint8_t> EncodeReply(const LanHostParams& params, const LanHostStatus& status)
		{
			const int max = std::clamp(status.SeatsMax, 1, MAX_SEATS);
			const int taken = std::clamp(status.SeatsTaken, 0, max);
			const size_t dataSize = (std::min)(status.GameData.size(), LAN_MAX_GAME_DATA);

			ByteWriter out;
			WriteHeader(out, TYPE_REPLY);
			out.U16(params.Protocol);
			out.U32(params.ContentHash);
			out.U16(params.GamePort);
			out.Str(params.Name);
			out.U8(static_cast<uint8_t>(taken));
			out.U8(static_cast<uint8_t>(max));
			out.U8(status.Accepting ? 1 : 0);
			out.U16(static_cast<uint16_t>(dataSize));
			std::vector<uint8_t> bytes = out.Data();
			bytes.insert(bytes.end(), status.GameData.begin(), status.GameData.begin() + static_cast<ptrdiff_t>(dataSize));
			return bytes;
		}

		bool DecodeQuery(std::span<const uint8_t> data)
		{
			ByteReader in(data);
			if (!ReadHeader(in, TYPE_QUERY))
			{
				return false;
			}
			in.U16();
			return in.Ok();
		}

		bool DecodeReply(std::span<const uint8_t> data, LanHostInfo& out, const std::function<std::string(std::string_view)>& filter)
		{
			ByteReader in(data);
			if (!ReadHeader(in, TYPE_REPLY))
			{
				return false;
			}
			out.Protocol = in.U16();
			out.ContentHash = in.U32();
			out.GamePort = in.U16();
			out.Name = FilterName(filter, in.Str());
			out.SeatsTaken = in.U8();
			out.SeatsMax = in.U8();
			const uint8_t accepting = in.U8();
			const size_t dataSize = in.U16();
			if (!in.Ok() || accepting > 1 || out.SeatsMax < 1 || out.SeatsMax > MAX_SEATS || out.SeatsTaken > out.SeatsMax
				|| dataSize > LAN_MAX_GAME_DATA || in.Remaining() != dataSize)
			{
				return false;
			}
			out.Accepting = accepting == 1;
			out.GameData.resize(dataSize);
			for (uint8_t& b : out.GameData)
			{
				b = in.U8();
			}
			return in.Ok();
		}

		std::string AddressText(const SocketAddress& address)
		{
			SocketAddress bare = address;
			bare.Port = 0;
			std::string text = FormatAddress(bare);
			return text.substr(0, text.rfind(':'));
		}

		SocketHandle OpenBound(const SocketAddress& address, SocketError& error)
		{
			const SocketHandle socket = OpenUdpSocket(AddressFamily::IPv4, error);
			if (socket == INVALID_SOCKET_HANDLE)
			{
				return socket;
			}
			error = Bind(socket, address);
			if (error != SocketError::None)
			{
				CloseSocket(socket);
				return INVALID_SOCKET_HANDLE;
			}
			return socket;
		}

	}

	std::unique_ptr<LanHost> LanHost::Open(const LanHostParams& params, std::string& error)
	{
		SocketAddress bind;
		if (!ParseAddress(params.BindAddress, params.Port, bind))
		{
			error = "bad bind address '" + params.BindAddress + "'";
			return nullptr;
		}
		if (!AcquireSocketLibrary(error))
		{
			return nullptr;
		}

		SocketError socketError = SocketError::None;
		const SocketHandle socket = OpenBound(bind, socketError);
		if (socket == INVALID_SOCKET_HANDLE)
		{
			error = "LAN host on port " + std::to_string(params.Port) + ": " + DescribeLastSocketError(socketError);
			ReleaseSocketLibrary();
			return nullptr;
		}

		SocketAddress local;
		GetLocalAddress(socket, local);

		std::unique_ptr<LanHost> host(new LanHost());
		host->m_Socket = socket;
		host->m_Port = local.Port;
		host->m_Params = params;
		host->m_Params.Name = FilterName(params.NameFilter, params.Name);
		LogInfo("LAN host '{}' answering on port {}", host->m_Params.Name, host->m_Port);
		return host;
	}

	LanHost::~LanHost()
	{
		if (m_Socket != INVALID_SOCKET_HANDLE)
		{
			CloseSocket(m_Socket);
			ReleaseSocketLibrary();
		}
	}

	void LanHost::SetStatus(const LanHostStatus& status)
	{
		m_Status = status;
		if (m_Status.GameData.size() > LAN_MAX_GAME_DATA)
		{
			m_Status.GameData.resize(LAN_MAX_GAME_DATA);
		}
	}

	void LanHost::Poll()
	{
		const std::vector<uint8_t> reply = EncodeReply(m_Params, m_Status);
		std::array<uint8_t, RECEIVE_BUFFER> buffer;
		int answers = 0;
		for (int i = 0; i < MAX_DATAGRAMS_PER_POLL; i++)
		{
			SocketAddress from;
			SocketError error = SocketError::None;
			const ptrdiff_t got = RecvFrom(m_Socket, buffer.data(), buffer.size(), from, error);
			if (got < 0)
			{
				if (error != SocketError::WouldBlock && error != SocketError::Other)
				{
					continue;
				}
				break;
			}

			const size_t size = static_cast<size_t>(got);
			if (size >= buffer.size() || size < reply.size() || !DecodeQuery({ buffer.data(), size }))
			{
				m_Malformed++;
				continue;
			}
			if (answers >= MAX_ANSWERS_PER_POLL)
			{
				m_Throttled++;
				continue;
			}

			answers++;
			if (SendTo(m_Socket, reply.data(), reply.size(), from, error) >= 0)
			{
				m_Answered++;
			}
		}
	}

	std::unique_ptr<LanBrowser> LanBrowser::Open(const LanBrowserParams& params, std::string& error)
	{
		SocketAddress target;
		if (!ParseAddress(params.QueryAddress, params.Port, target))
		{
			error = "bad query address '" + params.QueryAddress + "'";
			return nullptr;
		}
		if (!AcquireSocketLibrary(error))
		{
			return nullptr;
		}

		SocketAddress any;
		ParseAddress("0.0.0.0", 0, any);
		SocketError socketError = SocketError::None;
		const SocketHandle socket = OpenBound(any, socketError);
		if (socket == INVALID_SOCKET_HANDLE)
		{
			error = "LAN browser: " + DescribeLastSocketError(socketError);
			ReleaseSocketLibrary();
			return nullptr;
		}
		SetBroadcast(socket, true);
		SocketAddress local;
		GetLocalAddress(socket, local);

		std::unique_ptr<LanBrowser> browser(new LanBrowser());
		browser->m_Socket = socket;
		browser->m_Port = local.Port;
		browser->m_Params = params;
		browser->m_Start = params.Clock ? params.Clock() : std::chrono::steady_clock::now();
		return browser;
	}

	LanBrowser::~LanBrowser()
	{
		if (m_Socket != INVALID_SOCKET_HANDLE)
		{
			CloseSocket(m_Socket);
			ReleaseSocketLibrary();
		}
	}

	double LanBrowser::Now() const
	{
		const auto now = m_Params.Clock ? m_Params.Clock() : std::chrono::steady_clock::now();
		return std::chrono::duration<double>(now - m_Start).count();
	}

	void LanBrowser::Poll()
	{
		const double now = Now();

		if (now >= m_NextQuery)
		{
			SocketAddress target;
			ParseAddress(m_Params.QueryAddress, m_Params.Port, target);
			const std::vector<uint8_t> query = EncodeQuery(m_Params.Protocol);
			SocketError error = SocketError::None;
			if (SendTo(m_Socket, query.data(), query.size(), target, error) >= 0)
			{
				m_QueriesSent++;
			}
			else if (!m_SendFailureLogged)
			{
				m_SendFailureLogged = true;
				LogWarn("LAN browser: query not sent: {}", DescribeLastSocketError(error));
			}
			m_NextQuery = now + m_Params.QueryInterval;
		}

		std::array<uint8_t, RECEIVE_BUFFER> buffer;
		for (int i = 0; i < MAX_DATAGRAMS_PER_POLL; i++)
		{
			SocketAddress from;
			SocketError error = SocketError::None;
			const ptrdiff_t got = RecvFrom(m_Socket, buffer.data(), buffer.size(), from, error);
			if (got < 0)
			{
				if (error != SocketError::WouldBlock && error != SocketError::Other)
				{
					continue;
				}
				break;
			}

			LanHostInfo info;
			const size_t size = static_cast<size_t>(got);
			if (size >= buffer.size() || !DecodeReply({ buffer.data(), size }, info, m_Params.NameFilter))
			{
				m_Malformed++;
				continue;
			}

			info.Address = AddressText(from);
			info.Source = FormatAddress(from);
			info.ProtocolMatches = info.Protocol == m_Params.Protocol;
			info.ContentMatches = info.ContentHash == m_Params.ContentHash;

			const auto known = std::find_if(m_Hosts.begin(), m_Hosts.end(), [&](const LanHostInfo& h) { return h.Source == info.Source; });
			if (known != m_Hosts.end())
			{
				const size_t index = static_cast<size_t>(known - m_Hosts.begin());
				*known = std::move(info);
				m_LastSeen[index] = now;
			}
			else if (m_Hosts.size() < LAN_MAX_HOSTS)
			{
				m_Hosts.push_back(std::move(info));
				m_LastSeen.push_back(now);
			}
		}

		for (size_t i = m_Hosts.size(); i-- > 0;)
		{
			if (now - m_LastSeen[i] > m_Params.HostTimeout)
			{
				m_Hosts.erase(m_Hosts.begin() + static_cast<ptrdiff_t>(i));
				m_LastSeen.erase(m_LastSeen.begin() + static_cast<ptrdiff_t>(i));
			}
		}
	}

}
