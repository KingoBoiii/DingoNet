#pragma once

#include "DingoNet/Bytes.h"
#include "DingoNet/Transport.h"

#include <concepts>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Dingo::Net
{

	// The first byte of every packet is a message id: games own 0x00-0xEF, the session 0xF0-0xFF.
	inline constexpr uint8_t FIRST_SESSION_MESSAGE = 0xF0;

	constexpr bool IsSessionMessage(uint8_t id)
	{
		return id >= FIRST_SESSION_MESSAGE;
	}

	enum class Direction : uint8_t
	{
		ToHost,
		ToClients,
		Both
	};

	// Id (an integer or an enum, 0-255), Dir and Send are constants; Write and Read are the body (the id byte is the router's). Read returns
	// false for a body it cannot accept. An optional `static constexpr const char* Name` is used in logs.
	template<typename T>
	concept MessageIdType = std::integral<std::remove_cvref_t<decltype(T::Id)>> || std::is_enum_v<std::remove_cvref_t<decltype(T::Id)>>;

	template<typename T>
	concept NetMessage = MessageIdType<T> && std::default_initializable<T> && requires(const T& constMsg, T& msg, ByteWriter& writer, ByteReader& reader)
	{
		{ T::Dir } -> std::convertible_to<Direction>;
		{ T::Send } -> std::convertible_to<Delivery>;
		constMsg.Write(writer);
		{ msg.Read(reader) } -> std::convertible_to<bool>;
	};

	namespace Detail
	{

		template<NetMessage T>
		constexpr uint8_t IdOf()
		{
			constexpr long long raw = static_cast<long long>(T::Id);
			static_assert(raw >= 0 && raw <= 255, "a message Id must fit in one byte (0-255)");
			return static_cast<uint8_t>(raw);
		}

		void LogMessageError(std::string_view text);

		template<NetMessage T>
		std::string MessageName()
		{
			if constexpr (requires { { T::Name } -> std::convertible_to<const char*>; })
			{
				return T::Name;
			}
			else
			{
				return std::format("0x{:02X}", static_cast<unsigned>(IdOf<T>()));
			}
		}

	}

	// Id byte, then the body. An unreliable message over MAX_UNRELIABLE_BYTES fails here, at the sender, with
	// an error naming it.
	template<NetMessage T>
	std::optional<std::vector<uint8_t>> Encode(const T& message)
	{
		if (IsSessionMessage(Detail::IdOf<T>()))
		{
			Detail::LogMessageError(std::format("message {} has id 0x{:02X}, in the session's range (0xF0-0xFF); not sent",
				Detail::MessageName<T>(), static_cast<unsigned>(Detail::IdOf<T>())));
			return std::nullopt;
		}
		ByteWriter writer;
		writer.U8(Detail::IdOf<T>());
		message.Write(writer);
		if (static_cast<Delivery>(T::Send) == Delivery::Unreliable && writer.Size() > MAX_UNRELIABLE_BYTES)
		{
			Detail::LogMessageError(std::format("message {} is {} bytes encoded; an unreliable message may not exceed {}; not sent",
				Detail::MessageName<T>(), writer.Size(), MAX_UNRELIABLE_BYTES));
			return std::nullopt;
		}
		return writer.Data();
	}

	struct MessageStats
	{
		uint64_t Received = 0;
		uint64_t Sent = 0;
		uint64_t BytesIn = 0;
		uint64_t BytesOut = 0;
		uint64_t DroppedDirection = 0;
		uint64_t DroppedMalformed = 0;
	};

	// Received and BytesIn count messages whose handler ran; Sent and BytesOut count what the game reports
	// through NoteSent (Session's typed Send and Broadcast do, once a packet is handed to the transport).
	// BytesIn and BytesOut include the id byte. Not copyable or movable: a Session holds a pointer to it.
	class MessageRouter
	{
	public:
		MessageRouter();
		~MessageRouter();
		MessageRouter(const MessageRouter&) = delete;
		MessageRouter& operator=(const MessageRouter&) = delete;

		// False, with an error logged, for an id already registered or one in the session's range.
		template<NetMessage T, typename F>
			requires std::invocable<F&, int, const T&>
		bool On(F&& handler)
		{
			static_assert(std::copy_constructible<std::decay_t<F>>, "a message handler must be copy constructible (it is stored in a std::function)");
			return Add(Detail::IdOf<T>(), static_cast<Direction>(T::Dir), Detail::MessageName<T>(),
				[fn = std::forward<F>(handler)](int slot, std::span<const uint8_t> body) mutable
				{
					T message;
					ByteReader reader(body);
					if (!message.Read(reader) || !reader.Ok() || reader.Remaining() != 0)
					{
						return false;
					}
					fn(slot, std::as_const(message));
					return true;
				});
		}

		// Whether a handler ran. Unknown ids, a message at the wrong side and a malformed body are dropped and counted.
		bool Dispatch(int slot, std::span<const uint8_t> data, bool isHost);

		template<NetMessage T>
		void NoteSent(size_t bytes)
		{
			Entry& entry = m_Entries[Detail::IdOf<T>()];
			if (entry.Name.empty())
			{
				entry.Name = Detail::MessageName<T>();
			}
			entry.Stats.Sent++;
			entry.Stats.BytesOut += bytes;
		}

		const MessageStats& Stats(uint8_t id) const { return m_Entries[id].Stats; }
		bool Registered(uint8_t id) const { return m_Entries[id].Handler != nullptr; }
		const std::string& NameOf(uint8_t id) const { return m_Entries[id].Name; }
		// Every id with a handler or that was reported through NoteSent.
		std::vector<uint8_t> KnownIds() const;
		uint64_t DroppedUnknown() const { return m_DroppedUnknown; }
		void ResetStats();

	private:
		using HandlerFn = std::function<bool(int, std::span<const uint8_t>)>;

		struct Entry
		{
			HandlerFn Handler;
			Direction Dir = Direction::Both;
			std::string Name;
			MessageStats Stats;
		};

		bool Add(uint8_t id, Direction dir, std::string name, HandlerFn handler);

	private:
		std::unique_ptr<Entry[]> m_Entries;
		uint64_t m_DroppedUnknown = 0;
	};

}
