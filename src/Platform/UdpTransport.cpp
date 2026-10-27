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

		struct DatagramLoss
		{
			ENetHost* Host = nullptr;
			float Share = 0.0f;
			std::mt19937 Rng;
		};

		std::mutex s_LossMutex;
		std::vector<DatagramLoss> s_Loss;

		int ENET_CALLBACK DropDatagram(ENetHost* host, ENetEvent*)
		{
			std::lock_guard lock(s_LossMutex);
			for (DatagramLoss& loss : s_Loss)
			{
				if (loss.Host == host)
				{
					return std::uniform_real_distribution<float>(0.0f, 1.0f)(loss.Rng) < loss.Share ? 1 : 0;
				}
			}
			return 0;
		}

		void ClearDatagramLoss(ENetHost* host)
		{
			std::lock_guard lock(s_LossMutex);
			std::erase_if(s_Loss, [host](const DatagramLoss& loss) { return loss.Host == host; });
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
		};

		UdpParams Params;
		ENetHost* Host = nullptr;
		bool Library = false;
		std::vector<std::unique_ptr<PeerRecord>> Peers;
		PeerId NextId = 1;

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
	};

	UdpTransport::UdpTransport(const UdpParams& params)
		: m_Impl(std::make_unique<Impl>())
	{
		m_Impl->Params = params;
		m_Impl->Params.MaxReliableBytes = (std::max)(params.MaxReliableBytes, static_cast<size_t>(1));
		m_Impl->Params.MaxConnections = std::clamp(params.MaxConnections, 1, MAX_PEERS);
		m_Impl->Params.ReadBudgetBytes = (std::max)(params.ReadBudgetBytes, static_cast<size_t>(1));
	}

	std::unique_ptr<UdpTransport> UdpTransport::Listen(const std::string& address, uint16_t port, std::string& error, const UdpParams& params)
	{
		return Open(address, port, true, error, params);
	}

	std::unique_ptr<UdpTransport> UdpTransport::Connect(const std::string& address, uint16_t port, std::string& error, const UdpParams& params)
	{
		return Open(address, port, false, error, params);
	}

	std::unique_ptr<UdpTransport> UdpTransport::Open(const std::string& address, uint16_t port, bool listen, std::string& error, const UdpParams& params)
	{
		std::unique_ptr<UdpTransport> t(new UdpTransport(params));
		Impl& impl = *t->m_Impl;
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

		impl.Host = enet_host_create(listen ? &native : nullptr, listen ? static_cast<size_t>(impl.Params.MaxConnections) : 1, CHANNEL_COUNT, 0, 0);
		if (!impl.Host)
		{
			error = listen ? std::format("can't listen on {}:{}: the port is taken or not allowed", address, port) : std::string("no socket");
			return nullptr;
		}
		impl.Host->maximumWaitingData = (std::max)(impl.Params.ReadBudgetBytes, impl.Params.MaxReliableBytes * 2);
		impl.Host->maximumPacketSize = impl.Host->maximumWaitingData;
		Internal::MakeNonInheritable(static_cast<Internal::SocketHandle>(impl.Host->socket));

		if (listen)
		{
			t->m_Port = impl.Host->address.port;
			t->m_Describe = std::format("udp host {}:{}", address, t->m_Port);
			return t;
		}

		ENetPeer* link = enet_host_connect(impl.Host, &native, CHANNEL_COUNT, 0);
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
		if (!record || !record->Connected || record->Closing)
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
			Internal::LogWarn("udp: a packet of {} bytes was not queued", data.size());
			return;
		}
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

		size_t taken = 0;
		ENetEvent event;
		while (taken < impl.Params.ReadBudgetBytes)
		{
			const int result = enet_host_service(impl.Host, &event, 0);
			if (result < 0)
			{
				Internal::LogError("udp: the socket failed while receiving; closing the transport");
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
					peer->Link->data = nullptr;
				}
				impl.Peers.clear();
				Close();
				return;
			}
			if (result == 0)
			{
				break;
			}

			Impl::PeerRecord* record = static_cast<Impl::PeerRecord*>(event.peer->data);
			switch (event.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
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
					ENetPeer* link = record->Link;
					impl.Remove(record);
					enet_peer_disconnect_now(link, DISCONNECT_CLOSED);
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

			ClearDatagramLoss(impl.Host);
			enet_host_destroy(impl.Host);
			impl.Host = nullptr;
			impl.Peers.clear();
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
			ClearDatagramLoss(host);
			if (share <= 0.0f)
			{
				return;
			}
			{
				std::lock_guard lock(s_LossMutex);
				s_Loss.push_back(DatagramLoss{ host, share, std::mt19937(seed) });
			}
			host->intercept = &DropDatagram;
		}

	}

}
