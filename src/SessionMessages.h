#pragma once

#include "DingoNet/Session.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Dingo::Net::Internal
{

	enum class SessionMessage : uint8_t
	{
		Hello = 0xF0,
		Welcome = 0xF1,
		Reject = 0xF2,
		Ping = 0xF3,
		Pong = 0xF4,
		Bye = 0xF5,
		SeatJoined = 0xF6,
		SeatLeft = 0xF7,
		Epoch = 0xF8,
		Stamped = 0xF9
	};

	// Hello's session protocol (right after the id) and the whole Reject never change layout, so two
	// session protocols can still turn each other away.
	struct HelloMessage
	{
		uint16_t SessionProtocol = k_SessionProtocol;
		uint16_t Protocol = 0;
		uint32_t ContentHash = 0;
		std::string Name;
		std::string Build;
	};

	struct SeatEntry
	{
		uint8_t Slot = 0;
		std::string Name;
	};

	struct WelcomeMessage
	{
		uint8_t Slot = 0;
		uint8_t MaxSlots = 0;
		bool Epochs = false;
		uint8_t Epoch = 0;
		std::string HostName;
		std::vector<SeatEntry> Others;
	};

	struct RejectMessage
	{
		RejectReason Reason = RejectReason::ProtocolMismatch;
		uint16_t SessionProtocol = k_SessionProtocol;
		uint16_t Protocol = 0;
		uint32_t ContentHash = 0;
		std::string Build;
	};

	struct SeatLeftMessage
	{
		uint8_t Slot = 0;
		LeaveReason Reason = LeaveReason::Bye;
		DisconnectReason Transport = DisconnectReason::Closed;
	};

	std::vector<uint8_t> Encode(const HelloMessage& message);
	std::vector<uint8_t> Encode(const WelcomeMessage& message);
	std::vector<uint8_t> Encode(const RejectMessage& message);
	std::vector<uint8_t> Encode(const SeatEntry& joined);
	std::vector<uint8_t> Encode(const SeatLeftMessage& message);
	std::vector<uint8_t> EncodePing(uint32_t timeMs);
	std::vector<uint8_t> EncodePong(uint32_t timeMs);
	std::vector<uint8_t> EncodeBye(LeaveReason reason);
	std::vector<uint8_t> EncodeEpoch(uint8_t epoch);
	std::vector<uint8_t> EncodeStamped(uint8_t epoch, std::span<const uint8_t> game);

	// Each fails on a wrong id, a short read, trailing bytes or an out-of-range enum.
	bool DecodeHelloSessionProtocol(std::span<const uint8_t> packet, uint16_t& sessionProtocol);
	bool Decode(std::span<const uint8_t> packet, HelloMessage& out);
	bool Decode(std::span<const uint8_t> packet, WelcomeMessage& out);
	bool Decode(std::span<const uint8_t> packet, RejectMessage& out);
	bool Decode(std::span<const uint8_t> packet, SeatEntry& out);
	bool Decode(std::span<const uint8_t> packet, SeatLeftMessage& out);
	// Ping and Pong share a body.
	bool DecodePing(std::span<const uint8_t> packet, SessionMessage type, uint32_t& timeMs);
	bool DecodeBye(std::span<const uint8_t> packet, LeaveReason& reason);
	bool DecodeEpoch(std::span<const uint8_t> packet, uint8_t& epoch);
	// The game bytes (never empty) follow the two-byte header; fails on any other shape.
	bool DecodeStamped(std::span<const uint8_t> packet, uint8_t& epoch, std::span<const uint8_t>& game);

}
