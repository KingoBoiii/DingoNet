#pragma once

#include "DingoNet/Messages.h"
#include "DingoNet/Transport.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Dingo::Net
{

	// The session's own wire version: bumped only when its messages change. A game's Protocol and
	// ContentHash ride beside it in the Hello.
	inline constexpr uint16_t k_SessionProtocol = 2;
	inline constexpr int MAX_SESSION_SLOTS = 32;
	// With epochs on, every game message carries this many bytes of stamp.
	inline constexpr size_t EPOCH_STAMP_BYTES = 2;

	enum class RejectReason : uint8_t
	{
		ProtocolMismatch,
		ContentMismatch,
		NotAccepting,
		Full
	};

	enum class LeaveReason : uint8_t
	{
		Bye,
		TimedOut,
		Kicked,
		TransportLost
	};

	// Printable ASCII, leading and trailing spaces trimmed, at most 16 characters; "Player" if nothing is left.
	std::string DefaultNameFilter(std::string_view name);

	struct SessionParams
	{
		// Seats, the host's slot 0 included (1-32). A client takes a host's smaller count from the Welcome and
		// turns a larger one away (ProtocolMismatch), so no slot ever reaches past the game's own arrays.
		int MaxSlots = 4;
		uint16_t Protocol = 0;
		// Game-computed (map layouts, data tables): a mismatch turns the client away like a protocol one.
		uint32_t ContentHash = 0;
		// Informational: logged in a reject's Detail.
		std::string Build;
		double Timeout = 5.0;
		// At most Timeout / 2, or a quiet but healthy peer could time out between two pings.
		double PingInterval = 1.0;
		double HandshakeTimeout = 5.0;
		// Applied to every name the session takes in, its own included. Null: DefaultNameFilter.
		std::function<std::string(std::string_view)> NameFilter;
		// Null reads std::chrono::steady_clock; a test or a game's own net clock can drive it instead.
		std::function<std::chrono::steady_clock::time_point()> Clock;
		// Host: sees every game-range packet from a connection that has no seat yet. A reply is sent and the
		// connection hung up. Without it a client of an older wire (a Hello under another id) only times out,
		// so a game answers its own old Hello with its old reject here.
		std::function<std::optional<std::vector<uint8_t>>(std::span<const uint8_t>)> UnseatedPacket;
		// Host: game messages travel stamped with the session's epoch (see BeginEpoch), and a packet from another
		// epoch is dropped on arrival. A client takes the host's setting from the Welcome, so its own is only a hint
		// for MaxUnreliablePayload() until then.
		bool Epochs = false;
	};

	// Who's who over a Transport: the handshake (Hello -> Welcome or Reject), seats (slot 0 is the host),
	// pings with a smoothed round trip, timeouts and goodbyes. Game messages pass through untouched,
	// tagged with the sender's slot. Pumped by Update() once a frame: no threads.
	class Session
	{
	public:
		struct Event
		{
			enum class Kind : uint8_t
			{
				// Host: a client took Slot. Client: another client took Slot.
				Joined,
				// Host: Slot left or dropped (Leave). Client: another client did.
				Left,
				// Client: we're in as Slot; Name is the host's.
				Welcomed,
				// Client: turned away (Rejection); the session has stopped. Host: we turned Name away.
				Rejected,
				// Client: the host is gone (Leave); the session has stopped. Slot is -1 if we were never seated.
				Lost,
				// Client: the host began a new epoch (Epoch); the queued game messages were dropped. A game's
				// exhaustive switch over Kind needs a case for it or a default (-Wswitch).
				EpochChanged
			};

			Kind Type = Kind::Joined;
			int Slot = -1;
			std::string Name;
			RejectReason Rejection = RejectReason::ProtocolMismatch;
			LeaveReason Leave = LeaveReason::Bye;
			// Leave == TransportLost: what the transport that lost the peer reported: our own for a host's Left and a
			// client's Lost, the host's for a client's Left for another client (it rides in SeatLeft). Closed for any
			// other leave.
			Dingo::Net::DisconnectReason Transport = Dingo::Net::DisconnectReason::Closed;
			// Joined on a client: already seated when we were welcomed, not a new arrival.
			bool Existing = false;
			// Left on the host: the seat's game messages that were still queued when it went, oldest first.
			std::vector<std::vector<uint8_t>> LastMessages;
			// Lost: the reason came in the host's Bye. Lost(TimedOut) with it means the host stopped hearing from us
			// (our upstream); without it, we stopped hearing from the host.
			bool FromBye = false;
			// EpochChanged: the new epoch.
			uint8_t Epoch = 0;
			// For logs only: the transport's words, or both sides' versions on a reject.
			std::string Detail;
		};

		struct Message
		{
			int Slot = -1;
			std::vector<uint8_t> Data;
		};

		explicit Session(SessionParams params = {});
		~Session();

		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

		// Refused while active: the params a running session was started with stay.
		void SetParams(SessionParams params);
		const SessionParams& GetParams() const { return m_Params; }
		// Tests: claim a different protocol to exercise the reject. Takes effect at once.
		void SetProtocol(uint16_t protocol) { m_Params.Protocol = protocol; }

		void StartHost(std::unique_ptr<Transport> transport, std::string name);
		void StartClient(std::unique_ptr<Transport> transport, std::string name);
		// Says goodbye to everyone and closes. Raises no event.
		void Stop();
		// Host: sends the seat a goodbye and frees it (Left with Kicked here, Lost with Kicked there).
		void Kick(int slot);

		void Update();
		// A slot's game messages never outlast its Left: those still queued when it goes move to the Left's
		// LastMessages (a parting message sent just before a Stop arrives there), so none is ever read as the
		// next occupant's.
		// Take events before messages each frame: an EpochChanged comes before the new epoch's first messages
		// of the same Update, so the game clears its mirror state before applying them.
		std::vector<Event> TakeEvents() { return std::exchange(m_Events, {}); }
		std::vector<Message> TakeMessages() { return std::exchange(m_Messages, {}); }

		// Host with Epochs on: starts a new epoch. Every game message of the old one still in flight is dropped
		// wherever it lands, clients get an EpochChanged and the queued ones are dropped here and there, those
		// that arrived but were not taken yet included. So call BeginEpoch() first, then send the new state's
		// messages (a RunOver that must arrive goes after it, in the new epoch): one sent before may be dropped.
		// Call it at each state change that old packets must not outlive, and when a peer rejoining under the
		// same SteamID must not see its old traffic: that is the game's call. A client or Epochs off: a warning,
		// nothing done.
		void BeginEpoch();
		uint8_t GetEpoch() const { return m_Epoch; }
		// Started: the host's setting (a client's from the Welcome). Otherwise, and after Stop, the params'.
		bool EpochsOn() const { return m_EpochsOn; }
		// Stamped packets dropped for another epoch since Start: normal traffic around a BeginEpoch.
		uint64_t StaleDropped() const { return m_StaleDropped; }
		// The largest game packet an unreliable send may carry: MAX_UNRELIABLE_BYTES less the stamp when epochs
		// are on. A larger one logs an error and is dropped.
		size_t MaxUnreliablePayload() const { return m_EpochsOn ? MAX_UNRELIABLE_BYTES - EPOCH_STAMP_BYTES : MAX_UNRELIABLE_BYTES; }

		// Host: to one client's seat. Client: slot 0, the host. A first byte in the session's range is
		// an error and the packet is dropped.
		// Whether it was handed to the transport.
		bool Send(int slot, std::span<const uint8_t> data, Delivery delivery);
		// Host: every seated client but exceptSlot. Client: the host.
		// How many seats it was handed to the transport for.
		int Broadcast(std::span<const uint8_t> data, Delivery delivery, int exceptSlot = -1);

		// A message in its declared delivery. A host sending a ToHost message or a client a ToClients one is an
		// error and dropped. The router's Sent counters move only for a packet handed to the transport.
		template<NetMessage T>
		bool Send(int slot, const T& message)
		{
			if (!DirectionAllowed<T>())
			{
				return false;
			}
			std::optional<std::vector<uint8_t>> bytes = Encode(message);
			if (!bytes || !FitsUnreliable<T>(bytes->size()) || !Send(slot, *bytes, static_cast<Delivery>(T::Send)))
			{
				return false;
			}
			NoteSent<T>(bytes->size());
			return true;
		}

		// One send in the router's stats if it reached at least one seat, however many.
		template<NetMessage T>
		int Broadcast(const T& message, int exceptSlot = -1)
		{
			if (!DirectionAllowed<T>())
			{
				return 0;
			}
			std::optional<std::vector<uint8_t>> bytes = Encode(message);
			if (!bytes || !FitsUnreliable<T>(bytes->size()))
			{
				return 0;
			}
			const int reached = Broadcast(*bytes, static_cast<Delivery>(T::Send), exceptSlot);
			if (reached > 0)
			{
				NoteSent<T>(bytes->size());
			}
			return reached;
		}

		// Not owned; must outlive the session or be reset. Null: typed sends keep no counters.
		void SetRouter(MessageRouter* router) { m_Router = router; }
		MessageRouter* GetRouter() const { return m_Router; }

		bool Active() const { return m_Transport != nullptr; }
		bool IsHost() const { return m_IsHost; }
		// Host: always while active. Client: welcomed and seated.
		bool Connected() const { return m_LocalSlot >= 0; }
		int LocalSlot() const { return m_LocalSlot; }
		int MaxSlots() const { return static_cast<int>(m_Seats.size()); }
		int SeatedCount() const;
		bool SlotTaken(int slot) const;
		const std::string& NameOf(int slot) const;
		// Smoothed round trip to that seat; 0 before the first pong. A client pings only the host, so on a
		// client only slot 0 has one.
		float RttMs(int slot) const;
		// Host: turn joins away (a run is in progress).
		void SetAccepting(bool accepting) { m_Accepting = accepting; }
		bool IsAccepting() const { return m_Accepting; }
		// Connections held, seated or still in the handshake.
		size_t PeerCount() const { return m_Peers.size(); }
		// "tcp host 127.0.0.1:27960 | slot 0 | 2 seated | rtt 1:3ms | in 1.2 KB/s out 14.0 KB/s"
		std::string Describe() const;

	private:
		struct Peer
		{
			PeerId Id = 0;
			int Slot = -1;
			double LastHeard = 0.0;
			double ConnectedAt = 0.0;
			uint64_t Warned = 0;
			int BadPackets = 0;
			bool Heard = false;
		};

		enum class Problem : uint8_t
		{
			Malformed,
			Unexpected,
			Unknown
		};

		struct Seat
		{
			bool Taken = false;
			std::string Name;
			PeerId Connection = 0;
			float RttMs = 0.0f;
			bool HasRtt = false;
		};

		void Start(std::unique_ptr<Transport> transport, std::string name, bool host);
		void Shutdown();
		double Elapsed() const;
		uint32_t Ms(double seconds) const;
		std::string FilterName(std::string_view name) const;
		std::string DescribeVersions(uint16_t sessionProtocol, uint16_t protocol, uint32_t contentHash, std::string_view build) const;
		Peer* FindPeer(PeerId id);
		bool ValidClientSlot(int slot) const;

		void OnConnected(PeerId id, double now);
		void TrimHandshakes();
		bool OnDisconnected(const PeerEvent& e);
		void OnPacket(Peer& peer, Packet& packet);
		void OnUnseatedPacket(Peer& peer, std::span<const uint8_t> data);
		void OnHello(Peer& peer, std::span<const uint8_t> data);
		void OnWelcome(Peer& peer, std::span<const uint8_t> data);
		void OnReject(Peer& peer, std::span<const uint8_t> data);
		void OnPong(Peer& peer, std::span<const uint8_t> data);
		void OnBye(Peer& peer, std::span<const uint8_t> data);
		void OnSeatJoined(Peer& peer, std::span<const uint8_t> data);
		void OnSeatLeft(Peer& peer, std::span<const uint8_t> data);
		void OnEpoch(Peer& peer, std::span<const uint8_t> data);
		void OnStamped(Peer& peer, Packet& packet);
		void DropQueuedGameMessages();
		void OnGamePacket(Peer& peer, Packet& packet);
		bool UnreliableFits(size_t size, Delivery delivery) const;
		// Warns once per peer, problem and id and counts the rest; a host drops a peer that keeps at it.
		// May drop the peer: its caller returns at once.
		void Bad(Peer& peer, Problem problem, uint8_t id);

		void RejectPeer(PeerId id, RejectReason reason, std::string name, std::string detail);
		void DropPeer(PeerId id, LeaveReason reason, std::string detail = {}, DisconnectReason transport = DisconnectReason::Closed);
		void VacateSeat(int slot, LeaveReason reason, std::string detail, DisconnectReason transport);
		std::vector<uint8_t> Stamp(std::span<const uint8_t> data) const;
		void Lose(LeaveReason reason, std::string detail = {}, DisconnectReason transport = DisconnectReason::Closed);
		void SendToSeated(std::span<const uint8_t> data, int exceptSlot);

		template<NetMessage T>
		void NoteSent(size_t bytes)
		{
			if (m_Router)
			{
				m_Router->NoteSent<T>(bytes);
			}
		}

		template<NetMessage T>
		bool FitsUnreliable(size_t size) const
		{
			if (static_cast<Delivery>(T::Send) == Delivery::Unreliable && size > MaxUnreliablePayload())
			{
				Detail::LogMessageError(std::format("message {} is {} bytes encoded; an unreliable message may not exceed {} here; not sent",
					Detail::MessageName<T>(), size, MaxUnreliablePayload()));
				return false;
			}
			return true;
		}

		template<NetMessage T>
		bool DirectionAllowed() const
		{
			const Direction dir = static_cast<Direction>(T::Dir);
			if (Active() && ((dir == Direction::ToHost && m_IsHost) || (dir == Direction::ToClients && !m_IsHost)))
			{
				Detail::LogMessageError(std::format("message {} goes {} and cannot be sent by a {}; dropped",
					Detail::MessageName<T>(), dir == Direction::ToHost ? "to the host" : "to clients", m_IsHost ? "host" : "client"));
				return false;
			}
			return true;
		}

	private:
		SessionParams m_Params;
		std::unique_ptr<Transport> m_Transport;
		MessageRouter* m_Router = nullptr;
		bool m_IsHost = false;
		bool m_Accepting = true;
		bool m_EpochsOn = false;
		uint8_t m_Epoch = 0;
		uint64_t m_StaleDropped = 0;
		std::string m_Name;
		int m_LocalSlot = -1;
		std::vector<Peer> m_Peers;
		std::vector<Seat> m_Seats;
		std::vector<Event> m_Events;
		std::vector<Message> m_Messages;
		std::chrono::steady_clock::time_point m_StartTime{};
		double m_Now = 0.0;
		double m_NextPing = 0.0;
		double m_HandshakeStart = 0.0;
		double m_RateStart = 0.0;
		uint64_t m_RateIn = 0;
		uint64_t m_RateOut = 0;
		float m_InPerSec = 0.0f;
		float m_OutPerSec = 0.0f;
	};

}
