#include "DingoNet/Messages.h"

#include "Internal/Log.h"

namespace Dingo::Net
{

	namespace Detail
	{

		void LogMessageError(std::string_view text)
		{
			Internal::LogError("messages: {}", text);
		}

	}

	MessageRouter::MessageRouter() : m_Entries(std::make_unique<Entry[]>(256)) {}

	MessageRouter::~MessageRouter() = default;

	bool MessageRouter::Add(uint8_t id, Direction dir, std::string name, HandlerFn handler)
	{
		if (IsSessionMessage(id))
		{
			Internal::LogError("messages: {} has id 0x{:02X}, in the session's range (0xF0-0xFF); not registered", name, id);
			return false;
		}
		Entry& entry = m_Entries[id];
		if (entry.Handler)
		{
			Internal::LogError("messages: id 0x{:02X} ({}) is already registered as {}; not registered", id, name, entry.Name);
			return false;
		}
		entry.Handler = std::move(handler);
		entry.Dir = dir;
		entry.Name = std::move(name);
		return true;
	}

	bool MessageRouter::Dispatch(int slot, std::span<const uint8_t> data, bool isHost)
	{
		if (data.empty())
		{
			return false;
		}
		const uint8_t id = data[0];
		Entry& entry = m_Entries[id];
		if (!entry.Handler)
		{
			m_DroppedUnknown++;
			return false;
		}
		if ((entry.Dir == Direction::ToHost && !isHost) || (entry.Dir == Direction::ToClients && isHost))
		{
			entry.Stats.DroppedDirection++;
			return false;
		}
		if (!entry.Handler(slot, data.subspan(1)))
		{
			entry.Stats.DroppedMalformed++;
			return false;
		}
		entry.Stats.Received++;
		entry.Stats.BytesIn += data.size();
		return true;
	}

	std::vector<uint8_t> MessageRouter::KnownIds() const
	{
		std::vector<uint8_t> ids;
		for (size_t id = 0; id < 256; id++)
		{
			if (m_Entries[id].Handler || !m_Entries[id].Name.empty())
			{
				ids.push_back(static_cast<uint8_t>(id));
			}
		}
		return ids;
	}

	void MessageRouter::ResetStats()
	{
		for (size_t id = 0; id < 256; id++)
		{
			m_Entries[id].Stats = {};
		}
		m_DroppedUnknown = 0;
	}

}
