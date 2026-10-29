#include "DingoNet/UdpTransport.h"

#include "Internal/Log.h"
#include "Internal/SendLimits.h"
#include "Internal/UdpTestAccess.h"
#include "Platform/Socket.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <random>

namespace Dingo::Net
{

	namespace
	{

		using Clock = std::chrono::steady_clock;

		constexpr size_t CHANNEL_COUNT = 3;
		constexpr enet_uint8 CHANNEL_RELIABLE = 0;
		constexpr enet_uint8 CHANNEL_UNRELIABLE = 1;
		constexpr enet_uint8 CHANNEL_SEQUENCED = 2;
		constexpr enet_uint32 DISCONNECT_CLOSED = 1;
		constexpr double CLOSE_FLUSH_SECONDS = 0.25;
		constexpr double HANGUP_LINGER_SECONDS = 2.0;
		constexpr int MAX_PEERS = ENET_PROTOCOL_MAXIMUM_PEER_ID;
		// Charged per event against the read budget, so empty packets can't bypass it.
		constexpr size_t EVENT_COST_BYTES = 64;
		constexpr int MAX_FAILED_SERVICES_PER_POLL = 4;
		constexpr std::chrono::seconds SOCKET_FAILURE_CLOSE{ 1 };
		constexpr std::chrono::seconds SOCKET_FAILURE_WARN_INTERVAL{ 10 };
		// Matches the handshakes a session host keeps; a half-open connect holds a slot that ENet would keep ~31 s.
		constexpr size_t MAX_PENDING_CONNECTS = 16;

		std::mutex s_LibraryMutex;
		int s_LibraryUsers = 0;

		bool AcquireEnet(std::string& error)
		{
			std::lock_guard lock(s_LibraryMutex);
			if (s_LibraryUsers == 0 && enet_initialize() != 0)
			{
				error = "ENet failed to initialize";
				return false;
			}
			s_LibraryUsers++;
			return true;
		}

		void ReleaseEnet()
		{
			std::lock_guard lock(s_LibraryMutex);
			if (--s_LibraryUsers == 0)
			{
				enet_deinitialize();
			}
		}

		enet_uint32 Millis(double seconds)
		{
			const double clamped = std::isnan(seconds) ? 1.0 : std::clamp(seconds * 1000.0, 1.0, 3.6e6);
			return static_cast<enet_uint32>(clamped);
		}

		bool IsPendingConnect(const ENetPeer& peer)
		{
			return !peer.data && (peer.state == ENET_PEER_STATE_ACKNOWLEDGING_CONNECT || peer.state == ENET_PEER_STATE_CONNECTION_PENDING);
		}

		struct DatagramHook
		{
			ENetHost* Host = nullptr;
			float Share = 0.0f;
			std::mt19937 Rng;
			int Failures = 0;
		};

		std::mutex s_HookMutex;
		std::vector<DatagramHook> s_Hooks;

		int ENET_CALLBACK InterceptDatagram(ENetHost* host, ENetEvent*)
		{
			std::lock_guard lock(s_HookMutex);
			for (DatagramHook& hook : s_Hooks)
			{
				if (hook.Host == host)
				{
					if (hook.Failures > 0)
					{
						hook.Failures--;
						return -1;
					}
					return std::uniform_real_distribution<float>(0.0f, 1.0f)(hook.Rng) < hook.Share ? 1 : 0;
				}
			}
			return 0;
		}

		template<typename Change>
		void ChangeDatagramHook(ENetHost* host, Change change)
		{
			std::lock_guard lock(s_HookMutex);
			auto it = std::find_if(s_Hooks.begin(), s_Hooks.end(), [host](const DatagramHook& hook) { return hook.Host == host; });
			if (it == s_Hooks.end())
			{
				s_Hooks.push_back(DatagramHook{ host, 0.0f, std::mt19937(), 0 });
				it = s_Hooks.end() - 1;
			}
			change(*it);
			if (it->Share <= 0.0f && it->Failures <= 0)
			{
				s_Hooks.erase(it);
				host->intercept = nullptr;
				return;
			}
			host->intercept = &InterceptDatagram;
		}

		void ClearDatagramHook(ENetHost* host)
		{
			std::lock_guard lock(s_HookMutex);
			std::erase_if(s_Hooks, [host](const DatagramHook& hook) { return hook.Host == host; });
			if (host)
			{
				host->intercept = nullptr;
			}
		}

	}

	struct UdpTransport::Impl
	{
		struct PeerRecord
		{
			PeerId Id = 0;
			ENetPeer* Link = nullptr;
			bool Connected = false;
			// We hung up: nothing more is delivered from or to it, and its end raises no event.
			bool Closing = false;
			bool SendFailed = false;
			// Bytes of the packets ENet still holds for it. Their free callback points at this record, so a record is
			// destroyed only after ENet has reset its peer (or destroyed the host).
			size_t Queued = 0;
		};

		struct PendingConnect
		{
			ENetPeer* Link = nullptr;
			enet_uint32 ConnectId = 0;
			Clock::time_point Since;
		};

		UdpParams Params;
		size_t Channels = CHANNEL_COUNT;
		ENetHost* Host = nullptr;
		bool Library = false;
		std::vector<std::unique_ptr<PeerRecord>> Peers;
		// Oldest first.
		std::vector<PendingConnect> Pending;
		PeerId NextId = 1;
		std::optional<Clock::time_point> FailingSince;
		std::optional<Clock::time_point> LastFailureWarning;
		int FailuresSinceWarning = 0;

		static void ENET_CALLBACK Released(ENetPacket* packet)
		{
			PeerRecord* record = static_cast<PeerRecord*>(packet->userData);
			record->Queued -= (std::min)(record->Queued, packet->dataLength);
		}

		PeerRecord* Find(PeerId id)
		{
			for (auto& peer : Peers)
			{
				if (peer->Id == id)
				{
					return peer.get();
				}
			}
			return nullptr;
		}

		PeerRecord* Add(ENetPeer* link)
		{
			auto record = std::make_unique<PeerRecord>();
			record->Id = NextId++;
			record->Link = link;
			link->data = record.get();
			Peers.push_back(std::move(record));
			return Peers.back().get();
		}

		void Remove(PeerRecord* record)
		{
			record->Link->data = nullptr;
			std::erase_if(Peers, [record](const std::unique_ptr<PeerRecord>& peer) { return peer.get() == record; });
		}

		// ENet resets a peer that is not connected yet at once, without an event.
		void PruneReset()
		{
			std::erase_if(Peers, [](const std::unique_ptr<PeerRecord>& peer)
			{
				if (peer->Link->state != ENET_PEER_STATE_DISCONNECTED)
				{
					return false;
				}
				peer->Link->data = nullptr;
				return true;
			});
		}

		void Hangup(PeerRecord& record)
		{
			record.Closing = true;
			SetTimeout(record, HANGUP_LINGER_SECONDS);
			enet_peer_disconnect_later(record.Link, DISCONNECT_CLOSED);
		}

		void SetTimeout(PeerRecord& record, double seconds)
		{
			enet_peer_timeout(record.Link, 0, Millis(seconds), Millis(seconds));
		}

		// A send error to one unroutable address, or an ICMP-driven receive error, is a lost datagram, not a dead
		// socket: ENet's peer timeouts drop a peer that stays unreachable. True once every call has failed for 1 s.
		bool SocketFailed(Clock::time_point now, int code)
		{
			if (!FailingSince)
			{
				FailingSince = now;
			}
			FailuresSinceWarning++;
			if (!LastFailureWarning || now - *LastFailureWarning >= SOCKET_FAILURE_WARN_INTERVAL)
			{
				Internal::LogWarn("udp: a send or receive failed {} time(s) since the last warning (OS error {}); carrying on", FailuresSinceWarning, code);
				LastFailureWarning = now;
				FailuresSinceWarning = 0;
			}
			return now - *FailingSince >= SOCKET_FAILURE_CLOSE;
		}

		void DropOverfull(std::vector<PeerEvent>& events)
		{
			for (size_t i = 0; i < Peers.size();)
			{
				PeerRecord* record = Peers[i].get();
				if (record->Closing || record->Queued <= Params.MaxQueuedBytes)
				{
					i++;
					continue;
				}
				Internal::LogError("udp: dropped a peer with {} bytes queued (the limit is {})", record->Queued, Params.MaxQueuedBytes);
				PeerEvent dropped;
				dropped.Type = PeerEvent::Kind::Disconnected;
				dropped.Peer = record->Id;
				dropped.Reason = DisconnectReason::Error;
				dropped.Detail = std::format("{} bytes queued, over the limit of {}", record->Queued, Params.MaxQueuedBytes);
				events.push_back(std::move(dropped));
				enet_peer_disconnect_now(record->Link, DISCONNECT_CLOSED);
				Remove(record);
			}
		}

		void RefuseChannels(PeerRecord* record, ENetPeer* link, std::vector<PeerEvent>& events)
		{
			const size_t count = link->channelCount;
			enet_peer_disconnect_now(link, DISCONNECT_CLOSED);
			if (!record)
			{
				Internal::LogWarn("udp: refused a peer with {} channel(s) (DingoNet uses {})", count, Channels);
				return;
			}
			if (!record->Closing)
			{
				PeerEvent failed;
				failed.Type = PeerEvent::Kind::Disconnected;
				failed.Peer = record->Id;
				failed.Reason = DisconnectReason::ConnectFailed;
				failed.Detail = std::format("the host has {} channel(s), not {}", count, Channels);
				events.push_back(std::move(failed));
			}
			Remove(record);
		}

		void TrimPending(Clock::time_point now)
		{
			std::erase_if(Pending, [](const PendingConnect& pending)
			{
				return !IsPendingConnect(*pending.Link) || pending.Link->connectID != pending.ConnectId;
			});
			for (size_t i = 0; i < Host->peerCount; i++)
			{
				ENetPeer* peer = &Host->peers[i];
				if (IsPendingConnect(*peer) && std::none_of(Pending.begin(), Pending.end(), [peer](const PendingConnect& pending) { return pending.Link == peer; }))
				{
					Pending.push_back(PendingConnect{ peer, peer->connectID, now });
				}
			}
			const std::chrono::milliseconds limit(Millis(Params.ConnectTimeout));
			const size_t excess = Pending.size() > MAX_PENDING_CONNECTS ? Pending.size() - MAX_PENDING_CONNECTS : 0;
			for (size_t i = 0; i < Pending.size(); i++)
			{
				if (i < excess || now - Pending[i].Since >= limit)
				{
					enet_peer_reset(Pending[i].Link);
					Pending[i].Link = nullptr;
				}
			}
			std::erase_if(Pending, [](const PendingConnect& pending) { return !pending.Link; });
		}
	};

	UdpTransport::UdpTransport(const UdpParams& params)
		: m_Impl(std::make_unique<Impl>())
	{
		m_Impl->Params = params;
		m_Impl->Params.MaxReliableBytes = (std::max)(params.MaxReliableBytes, static_cast<size_t>(1));
		m_Impl->Params.MaxQueuedBytes = (std::max)(params.MaxQueuedBytes, m_Impl->Params.MaxReliableBytes);
		m_Impl->Params.MaxConnections = std::clamp(params.MaxConnections, 1, MAX_PEERS);
		m_Impl->Params.MaxConnectionsPerAddress = std::clamp(params.MaxConnectionsPerAddress, 1, m_Impl->Params.MaxConnections);
		m_Impl->Params.ReadBudgetBytes = (std::max)(params.ReadBudgetBytes, static_cast<size_t>(1));
	}

	std::unique_ptr<UdpTransport> UdpTransport::Listen(const std::string& address, uint16_t port, std::string& error, const UdpParams& params)
	{
		return Open(address, port, true, CHANNEL_COUNT, error, params);
	}

	std::unique_ptr<UdpTransport> UdpTransport::Connect(const std::string& address, uint16_t port, std::string& error, const UdpParams& params)
	{
		return Open(address, port, false, CHANNEL_COUNT, error, params);
	}

	std::unique_ptr<UdpTransport> UdpTransport::Open(const std::string& address, uint16_t port, bool listen, size_t channels, std::string& error, const UdpParams& params)
	{
		std::unique_ptr<UdpTransport> t(new UdpTransport(params));
		Impl& impl = *t->m_Impl;
		impl.Channels = std::clamp(channels, static_cast<size_t>(1), CHANNEL_COUNT);
		if (!AcquireEnet(error))
		{
			return nullptr;
		}
		impl.Library = true;

		Internal::SocketAddress parsed;
		ENetAddress native{};
		if (!Internal::ParseAddress(address, port, parsed) || enet_address_set_host_ip(&native, address.c_str()) != 0)
		{
			error = std::format("'{}' is not an IPv4 address", address);
			return nullptr;
		}
		native.port = port;

		impl.Host = enet_host_create(listen ? &native : nullptr, listen ? static_cast<size_t>(impl.Params.MaxConnections) : 1, impl.Channels, 0, 0);
		if (!impl.Host)
		{
			error = listen ? std::format("can't listen on {}:{}: the port is taken or not allowed", address, port) : std::string("no socket");
			return nullptr;
		}
		impl.Host->maximumWaitingData = (std::max)(impl.Params.ReadBudgetBytes, impl.Params.MaxReliableBytes * 2);
		impl.Host->maximumPacketSize = impl.Host->maximumWaitingData;
		impl.Host->duplicatePeers = static_cast<size_t>(impl.Params.MaxConnectionsPerAddress);
		const Internal::SocketHandle socket = static_cast<Internal::SocketHandle>(impl.Host->socket);
		Internal::MakeNonInheritable(socket);
		Internal::DisableUdpConnReset(socket);

		if (listen)
		{
			t->m_Port = impl.Host->address.port;
			t->m_Describe = std::format("udp host {}:{}", address, t->m_Port);
			return t;
		}

		ENetPeer* link = enet_host_connect(impl.Host, &native, impl.Channels, 0);
		if (!link)
		{
			error = "no free connection";
			return nullptr;
		}
		impl.SetTimeout(*impl.Add(link), impl.Params.ConnectTimeout);
		t->m_Port = port;
		t->m_Describe = std::format("udp client -> {}:{}", address, port);
		return t;
	}

	UdpTransport::~UdpTransport()
	{
		Close();
	}

	void UdpTransport::Send(PeerId to, std::span<const uint8_t> data, Delivery delivery)
	{
		Impl& impl = *m_Impl;
		if (!impl.Host || !Internal::CheckUnreliableSize(data.size(), delivery, "udp"))
		{
			return;
		}
		if (delivery == Delivery::Reliable && data.size() > impl.Params.MaxReliableBytes)
		{
			Internal::LogError("udp: dropped a reliable packet of {} bytes (the limit is {})", data.size(), impl.Params.MaxReliableBytes);
			return;
		}

		Impl::PeerRecord* record = impl.Find(to);
		if (!record || !record->Connected || record->Closing || record->Queued > impl.Params.MaxQueuedBytes)
		{
			return;
		}

		enet_uint8 channel = CHANNEL_RELIABLE;
		enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
		if (delivery == Delivery::Unreliable)
		{
			channel = CHANNEL_UNRELIABLE;
			flags = ENET_PACKET_FLAG_UNSEQUENCED;
		}
		else if (delivery == Delivery::UnreliableSequenced)
		{
			channel = CHANNEL_SEQUENCED;
			flags = 0;
		}

		ENetPacket* packet = enet_packet_create(data.data(), data.size(), flags);
		if (!packet)
		{
			Internal::LogError("udp: out of memory for a packet of {} bytes", data.size());
			return;
		}
		if (enet_peer_send(record->Link, channel, packet) != 0)
		{
			enet_packet_destroy(packet);
			if (!record->SendFailed)
			{
				record->SendFailed = true;
				Internal::LogWarn("udp: a packet of {} bytes to peer {} was not queued; later failures for it are not logged", data.size(), to);
			}
			return;
		}
		packet->userData = record;
		packet->freeCallback = &Impl::Released;
		record->Queued += data.size();
		BytesOut += data.size();
	}

	void UdpTransport::Poll(std::vector<Packet>& packets, std::vector<PeerEvent>& events)
	{
		Impl& impl = *m_Impl;
		if (!impl.Host)
		{
			return;
		}
		enet_host_flush(impl.Host);
		impl.DropOverfull(events);

		size_t taken = 0;
		int failures = 0;
		ENetEvent event;
		while (taken < impl.Params.ReadBudgetBytes)
		{
			const int result = enet_host_service(impl.Host, &event, 0);
			if (result < 0)
			{
				const int code = Internal::CurrentNativeSocketError();
				if (impl.SocketFailed(Clock::now(), code))
				{
					Internal::LogError("udp: every send and receive failed for 1 s (OS error {}); closing the transport", code);
					for (auto& peer : impl.Peers)
					{
						if (!peer->Closing)
						{
							PeerEvent failed;
							failed.Type = PeerEvent::Kind::Disconnected;
							failed.Peer = peer->Id;
							failed.Reason = peer->Connected ? DisconnectReason::Error : DisconnectReason::ConnectFailed;
							failed.Detail = "the socket failed";
							events.push_back(std::move(failed));
						}
					}
					Close();
					return;
				}
				if (++failures >= MAX_FAILED_SERVICES_PER_POLL)
				{
					break;
				}
				continue;
			}
			impl.FailingSince.reset();
			if (result == 0)
			{
				break;
			}
			taken += EVENT_COST_BYTES;

			Impl::PeerRecord* record = static_cast<Impl::PeerRecord*>(event.peer->data);
			switch (event.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				if (event.peer->channelCount < impl.Channels)
				{
					impl.RefuseChannels(record, event.peer, events);
					break;
				}
				if (!record)
				{
					record = impl.Add(event.peer);
				}
				record->Connected = true;
				impl.SetTimeout(*record, impl.Params.PeerTimeout);
				PeerEvent connected;
				connected.Type = PeerEvent::Kind::Connected;
				connected.Peer = record->Id;
				events.push_back(std::move(connected));
				break;
			}
			case ENET_EVENT_TYPE_RECEIVE:
			{
				const size_t size = event.packet->dataLength;
				taken += size;
				if (record && !record->Closing && size > impl.Params.MaxReliableBytes)
				{
					Internal::LogError("udp: dropped a peer that sent {} bytes (the limit is {})", size, impl.Params.MaxReliableBytes);
					PeerEvent oversize;
					oversize.Type = PeerEvent::Kind::Disconnected;
					oversize.Peer = record->Id;
					oversize.Reason = DisconnectReason::Error;
					oversize.Detail = std::format("a packet of {} bytes is over the limit of {}", size, impl.Params.MaxReliableBytes);
					events.push_back(std::move(oversize));
					enet_peer_disconnect_now(record->Link, DISCONNECT_CLOSED);
					impl.Remove(record);
				}
				else if (record && !record->Closing)
				{
					Packet packet;
					packet.From = record->Id;
					packet.Data.assign(event.packet->data, event.packet->data + size);
					packets.push_back(std::move(packet));
					BytesIn += size;
				}
				enet_packet_destroy(event.packet);
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				if (!record)
				{
					break;
				}
				const bool raise = !record->Closing;
				PeerEvent gone;
				gone.Type = PeerEvent::Kind::Disconnected;
				gone.Peer = record->Id;
				if (!record->Connected)
				{
					gone.Reason = DisconnectReason::ConnectFailed;
					gone.Detail = std::format("no answer within {:.1f} s", impl.Params.ConnectTimeout);
				}
				else if (event.data == DISCONNECT_CLOSED)
				{
					gone.Reason = DisconnectReason::Closed;
					gone.Detail = "closed by the other end";
				}
				else
				{
					gone.Reason = DisconnectReason::TimedOut;
					gone.Detail = std::format("silent for {:.1f} s", impl.Params.PeerTimeout);
				}
				impl.Remove(record);
				if (raise)
				{
					events.push_back(std::move(gone));
				}
				break;
			}
			default:
				break;
			}
		}
		impl.TrimPending(Clock::now());
	}

	void UdpTransport::Disconnect(PeerId peer)
	{
		Impl& impl = *m_Impl;
		if (!impl.Host)
		{
			return;
		}
		Impl::PeerRecord* record = impl.Find(peer);
		if (!record || record->Closing)
		{
			return;
		}
		impl.Hangup(*record);
		impl.PruneReset();
	}

	void UdpTransport::Close()
	{
		Impl& impl = *m_Impl;
		if (impl.Host)
		{
			for (auto& peer : impl.Peers)
			{
				if (!peer->Closing)
				{
					impl.Hangup(*peer);
				}
			}
			impl.PruneReset();

			const Clock::time_point deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(CLOSE_FLUSH_SECONDS));
			ENetEvent event;
			while (!impl.Peers.empty() && Clock::now() < deadline)
			{
				int result = enet_host_service(impl.Host, &event, 5);
				for (; result > 0; result = enet_host_service(impl.Host, &event, 0))
				{
					if (event.type == ENET_EVENT_TYPE_RECEIVE)
					{
						enet_packet_destroy(event.packet);
					}
					else if (event.type == ENET_EVENT_TYPE_CONNECT && !event.peer->data)
					{
						enet_peer_disconnect_now(event.peer, DISCONNECT_CLOSED);
					}
					else if (event.type == ENET_EVENT_TYPE_DISCONNECT && event.peer->data)
					{
						impl.Remove(static_cast<Impl::PeerRecord*>(event.peer->data));
					}
				}
				if (result < 0)
				{
					break;
				}
			}
			for (auto& peer : impl.Peers)
			{
				enet_peer_disconnect_now(peer->Link, DISCONNECT_CLOSED);
			}
			enet_host_flush(impl.Host);

			ClearDatagramHook(impl.Host);
			enet_host_destroy(impl.Host);
			impl.Host = nullptr;
			impl.Peers.clear();
			impl.Pending.clear();
		}
		if (impl.Library)
		{
			impl.Library = false;
			ReleaseEnet();
		}
	}

	namespace Internal
	{

		void UdpTestAccess::DropIncomingDatagrams(UdpTransport& transport, float share, uint32_t seed)
		{
			ENetHost* host = transport.m_Impl->Host;
			if (!host)
			{
				return;
			}
			ChangeDatagramHook(host, [share, seed](DatagramHook& hook)
			{
				hook.Share = share;
				hook.Rng.seed(seed);
			});
		}

		void UdpTestAccess::FailIncomingDatagrams(UdpTransport& transport, int count)
		{
			ENetHost* host = transport.m_Impl->Host;
			if (!host)
			{
				return;
			}
			ChangeDatagramHook(host, [count](DatagramHook& hook) { hook.Failures = count; });
		}

		void UdpTestAccess::BreakSocket(UdpTransport& transport)
		{
			ENetHost* host = transport.m_Impl->Host;
			if (!host)
			{
				return;
			}
			enet_socket_destroy(host->socket);
			host->socket = ENET_SOCKET_NULL;
		}

		bool UdpTestAccess::IsOpen(const UdpTransport& transport)
		{
			return transport.m_Impl->Host != nullptr;
		}

		int UdpTestAccess::PendingPeers(const UdpTransport& transport)
		{
			const ENetHost* host = transport.m_Impl->Host;
			if (!host)
			{
				return 0;
			}
			return static_cast<int>(std::count_if(host->peers, host->peers + host->peerCount, [](const ENetPeer& peer) { return IsPendingConnect(peer); }));
		}

		size_t UdpTestAccess::QueuedBytes(const UdpTransport& transport, PeerId peer)
		{
			const UdpTransport::Impl::PeerRecord* record = transport.m_Impl->Find(peer);
			return record ? record->Queued : 0;
		}

		std::unique_ptr<UdpTransport> UdpTestAccess::Open(const std::string& address, uint16_t port, bool listen, size_t channels, std::string& error, const UdpParams& params)
		{
			return UdpTransport::Open(address, port, listen, channels, error, params);
		}

	}

}
