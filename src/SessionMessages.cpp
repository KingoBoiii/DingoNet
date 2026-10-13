#include "SessionMessages.h"

#include "DingoNet/Bytes.h"

#include <utility>

namespace Dingo::Net::Internal
{

	namespace
	{

		ByteWriter Begin(SessionMessage type)
		{
			ByteWriter w;
			w.U8(static_cast<uint8_t>(type));
			return w;
		}

		bool Is(ByteReader& r, SessionMessage type)
		{
			return r.U8() == static_cast<uint8_t>(type) && r.Ok();
		}

		bool Done(const ByteReader& r)
		{
			return r.Ok() && r.Remaining() == 0;
		}

		bool ReadRejectReason(ByteReader& r, RejectReason& out)
		{
			const uint8_t value = r.U8();
			out = static_cast<RejectReason>(value);
			return value <= static_cast<uint8_t>(RejectReason::Full);
		}

		bool ReadLeaveReason(ByteReader& r, LeaveReason& out)
		{
			const uint8_t value = r.U8();
			out = static_cast<LeaveReason>(value);
			return value <= static_cast<uint8_t>(LeaveReason::TransportLost);
		}

	}

	std::vector<uint8_t> Encode(const HelloMessage& message)
	{
		ByteWriter w = Begin(SessionMessage::Hello);
		w.U16(message.SessionProtocol);
		w.U16(message.Protocol);
		w.U32(message.ContentHash);
		w.Str(message.Name);
		w.Str(message.Build);
		return w.Data();
	}

	std::vector<uint8_t> Encode(const WelcomeMessage& message)
	{
		ByteWriter w = Begin(SessionMessage::Welcome);
		w.U8(message.Slot);
		w.U8(message.MaxSlots);
		w.Str(message.HostName);
		w.U8(static_cast<uint8_t>(message.Others.size()));
		for (const SeatEntry& seat : message.Others)
		{
			w.U8(seat.Slot);
			w.Str(seat.Name);
		}
		return w.Data();
	}

	std::vector<uint8_t> Encode(const RejectMessage& message)
	{
		ByteWriter w = Begin(SessionMessage::Reject);
		w.U8(static_cast<uint8_t>(message.Reason));
		w.U16(message.SessionProtocol);
		w.U16(message.Protocol);
		w.U32(message.ContentHash);
		w.Str(message.Build);
		return w.Data();
	}

	std::vector<uint8_t> Encode(const SeatEntry& joined)
	{
		ByteWriter w = Begin(SessionMessage::SeatJoined);
		w.U8(joined.Slot);
		w.Str(joined.Name);
		return w.Data();
	}

	std::vector<uint8_t> Encode(const SeatLeftMessage& message)
	{
		ByteWriter w = Begin(SessionMessage::SeatLeft);
		w.U8(message.Slot);
		w.U8(static_cast<uint8_t>(message.Reason));
		return w.Data();
	}

	std::vector<uint8_t> EncodePing(uint32_t timeMs)
	{
		ByteWriter w = Begin(SessionMessage::Ping);
		w.U32(timeMs);
		return w.Data();
	}

	std::vector<uint8_t> EncodePong(uint32_t timeMs)
	{
		ByteWriter w = Begin(SessionMessage::Pong);
		w.U32(timeMs);
		return w.Data();
	}

	std::vector<uint8_t> EncodeBye(LeaveReason reason)
	{
		ByteWriter w = Begin(SessionMessage::Bye);
		w.U8(static_cast<uint8_t>(reason));
		return w.Data();
	}

	bool DecodeHelloSessionProtocol(std::span<const uint8_t> packet, uint16_t& sessionProtocol)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::Hello))
		{
			return false;
		}
		sessionProtocol = r.U16();
		return r.Ok();
	}

	bool Decode(std::span<const uint8_t> packet, HelloMessage& out)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::Hello))
		{
			return false;
		}
		out.SessionProtocol = r.U16();
		out.Protocol = r.U16();
		out.ContentHash = r.U32();
		out.Name = r.Str();
		out.Build = r.Str();
		return Done(r);
	}

	bool Decode(std::span<const uint8_t> packet, WelcomeMessage& out)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::Welcome))
		{
			return false;
		}
		out.Slot = r.U8();
		out.MaxSlots = r.U8();
		out.HostName = r.Str();
		const uint8_t count = r.U8();
		if (!r.Ok() || count > MAX_SESSION_SLOTS)
		{
			return false;
		}
		out.Others.clear();
		for (uint8_t i = 0; i < count; i++)
		{
			SeatEntry seat;
			seat.Slot = r.U8();
			seat.Name = r.Str();
			out.Others.push_back(std::move(seat));
		}
		return Done(r);
	}

	bool Decode(std::span<const uint8_t> packet, RejectMessage& out)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::Reject) || !ReadRejectReason(r, out.Reason))
		{
			return false;
		}
		out.SessionProtocol = r.U16();
		out.Protocol = r.U16();
		out.ContentHash = r.U32();
		out.Build = r.Str();
		return Done(r);
	}

	bool Decode(std::span<const uint8_t> packet, SeatEntry& out)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::SeatJoined))
		{
			return false;
		}
		out.Slot = r.U8();
		out.Name = r.Str();
		return Done(r);
	}

	bool Decode(std::span<const uint8_t> packet, SeatLeftMessage& out)
	{
		ByteReader r(packet);
		if (!Is(r, SessionMessage::SeatLeft))
		{
			return false;
		}
		out.Slot = r.U8();
		return ReadLeaveReason(r, out.Reason) && Done(r);
	}

	bool DecodePing(std::span<const uint8_t> packet, SessionMessage type, uint32_t& timeMs)
	{
		ByteReader r(packet);
		if (!Is(r, type))
		{
			return false;
		}
		timeMs = r.U32();
		return Done(r);
	}

	bool DecodeBye(std::span<const uint8_t> packet, LeaveReason& reason)
	{
		ByteReader r(packet);
		return Is(r, SessionMessage::Bye) && ReadLeaveReason(r, reason) && Done(r);
	}

}
