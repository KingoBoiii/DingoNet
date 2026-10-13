#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Dingo::Net
{

	class ByteWriter
	{
	public:
		void U8(uint8_t value) { m_Data.push_back(value); }
		void U16(uint16_t value);
		void U32(uint32_t value);
		void U64(uint64_t value);
		void I16(int16_t value) { U16(static_cast<uint16_t>(value)); }
		void F32(float value);
		void Str(std::string_view text);

		void Coord(float metres);
		void Angle16(float degrees);
		void Angle8(float degrees);
		void Unit8(float fraction);

		const std::vector<uint8_t>& Data() const { return m_Data; }
		size_t Size() const { return m_Data.size(); }
		void Clear() { m_Data.clear(); }

	private:
		std::vector<uint8_t> m_Data;
	};

	// Reading past the end yields zeros and marks the reader failed for good: check Ok() once at the end.
	class ByteReader
	{
	public:
		explicit ByteReader(std::span<const uint8_t> data) : m_Data(data) {}
		ByteReader(const uint8_t* data, size_t size) : m_Data(data, size) {}
		explicit ByteReader(const std::vector<uint8_t>& data) : m_Data(data) {}
		explicit ByteReader(const std::vector<uint8_t>&&) = delete;

		uint8_t U8();
		uint16_t U16();
		uint32_t U32();
		uint64_t U64();
		int16_t I16() { return static_cast<int16_t>(U16()); }
		float F32();
		std::string Str();

		float Coord() { return I16() / 100.0f; }
		float Angle16() { return U16() * (360.0f / 65536.0f); }
		float Angle8() { return U8() * (360.0f / 256.0f); }
		float Unit8() { return U8() / 255.0f; }

		bool Ok() const { return !m_Failed; }
		size_t Remaining() const { return m_Data.size() - m_Position; }

	private:
		bool Need(size_t bytes);

	private:
		std::span<const uint8_t> m_Data;
		size_t m_Position = 0;
		bool m_Failed = false;
	};

	uint8_t PeekU8(std::span<const uint8_t> packet);

}
