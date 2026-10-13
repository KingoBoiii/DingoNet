#include "DingoNet/Bytes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Dingo::Net
{

	namespace
	{

		float Wrap360(float degrees)
		{
			if (!std::isfinite(degrees))
			{
				return 0.0f;
			}
			const float wrapped = std::fmod(degrees, 360.0f);
			return wrapped < 0.0f ? wrapped + 360.0f : wrapped;
		}

		float ZeroIfNan(float value)
		{
			return std::isnan(value) ? 0.0f : value;
		}

		template<typename T>
		T ReadLittleEndian(const uint8_t* bytes)
		{
			T value = 0;
			for (size_t i = 0; i < sizeof(T); i++)
			{
				value |= static_cast<T>(static_cast<T>(bytes[i]) << (i * 8));
			}
			return value;
		}

	}

	void ByteWriter::U16(uint16_t value)
	{
		m_Data.push_back(static_cast<uint8_t>(value));
		m_Data.push_back(static_cast<uint8_t>(value >> 8));
	}

	void ByteWriter::U32(uint32_t value)
	{
		U16(static_cast<uint16_t>(value));
		U16(static_cast<uint16_t>(value >> 16));
	}

	void ByteWriter::U64(uint64_t value)
	{
		U32(static_cast<uint32_t>(value));
		U32(static_cast<uint32_t>(value >> 32));
	}

	void ByteWriter::F32(float value)
	{
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		U32(bits);
	}

	void ByteWriter::Str(std::string_view text)
	{
		const size_t length = (std::min)(text.size(), size_t{ 255 });
		U8(static_cast<uint8_t>(length));
		m_Data.insert(m_Data.end(), text.begin(), text.begin() + length);
	}

	void ByteWriter::Coord(float metres)
	{
		const float centimetres = std::clamp(ZeroIfNan(metres) * 100.0f, -32768.0f, 32767.0f);
		I16(static_cast<int16_t>(std::lround(centimetres)));
	}

	void ByteWriter::Angle16(float degrees)
	{
		U16(static_cast<uint16_t>(std::lround(Wrap360(degrees) / 360.0f * 65536.0f) & 0xFFFF));
	}

	void ByteWriter::Angle8(float degrees)
	{
		U8(static_cast<uint8_t>(std::lround(Wrap360(degrees) / 360.0f * 256.0f) & 0xFF));
	}

	void ByteWriter::Unit8(float fraction)
	{
		U8(static_cast<uint8_t>(std::lround(std::clamp(ZeroIfNan(fraction), 0.0f, 1.0f) * 255.0f)));
	}

	bool ByteReader::Need(size_t bytes)
	{
		if (m_Failed || Remaining() < bytes)
		{
			m_Failed = true;
			return false;
		}
		return true;
	}

	uint8_t ByteReader::U8()
	{
		if (!Need(1))
		{
			return 0;
		}
		return m_Data[m_Position++];
	}

	uint16_t ByteReader::U16()
	{
		if (!Need(2))
		{
			return 0;
		}
		const uint16_t value = ReadLittleEndian<uint16_t>(m_Data.data() + m_Position);
		m_Position += 2;
		return value;
	}

	uint32_t ByteReader::U32()
	{
		if (!Need(4))
		{
			return 0;
		}
		const uint32_t value = ReadLittleEndian<uint32_t>(m_Data.data() + m_Position);
		m_Position += 4;
		return value;
	}

	uint64_t ByteReader::U64()
	{
		if (!Need(8))
		{
			return 0;
		}
		const uint64_t value = ReadLittleEndian<uint64_t>(m_Data.data() + m_Position);
		m_Position += 8;
		return value;
	}

	float ByteReader::F32()
	{
		const uint32_t bits = U32();
		float value;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	std::string ByteReader::Str()
	{
		const size_t length = U8();
		if (!Need(length))
		{
			return {};
		}
		std::string text(reinterpret_cast<const char*>(m_Data.data() + m_Position), length);
		m_Position += length;
		return text;
	}

	uint8_t PeekU8(std::span<const uint8_t> packet)
	{
		return packet.empty() ? 0 : packet[0];
	}

}
