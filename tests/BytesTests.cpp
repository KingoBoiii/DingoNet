#include "TestFramework.h"

#include <DingoNet/Bytes.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace Dingo::Net;

namespace
{

	bool Near(float a, float b, float tolerance)
	{
		return std::fabs(a - b) <= tolerance;
	}

	float AngleDistance(float a, float b)
	{
		const float d = std::fmod(std::fabs(a - b), 360.0f);
		return (std::min)(d, 360.0f - d);
	}

	uint32_t Bits(float value)
	{
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

}

TEST(BytesIntegersRoundTripAtTheirEdges)
{
	ByteWriter w;
	w.U8(0);
	w.U8(255);
	w.U16(0);
	w.U16(0xFFFF);
	w.U32(0);
	w.U32(0xFFFFFFFFu);
	w.U64(0);
	w.U64((std::numeric_limits<uint64_t>::max)());
	w.I16(-32768);
	w.I16(32767);
	w.I16(-1);
	CHECK(w.Size() == 2 + 4 + 8 + 16 + 6);

	ByteReader r(w.Data());
	CHECK(r.U8() == 0);
	CHECK(r.U8() == 255);
	CHECK(r.U16() == 0);
	CHECK(r.U16() == 0xFFFF);
	CHECK(r.U32() == 0);
	CHECK(r.U32() == 0xFFFFFFFFu);
	CHECK(r.U64() == 0);
	CHECK(r.U64() == (std::numeric_limits<uint64_t>::max)());
	CHECK(r.I16() == -32768);
	CHECK(r.I16() == 32767);
	CHECK(r.I16() == -1);
	CHECK(r.Ok());
	CHECK(r.Remaining() == 0);
}

TEST(BytesWireLayoutIsLittleEndianAndStable)
{
	ByteWriter w;
	w.U32(0x04030201u);
	w.U64(0x0807060504030201ull);
	w.F32(1.0f);
	w.Str("ab");
	w.Coord(1.5f);
	w.Angle16(180.0f);
	w.Angle8(90.0f);
	w.Unit8(1.0f);

	const std::vector<uint8_t> expected = {
		0x01, 0x02, 0x03, 0x04,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x00, 0x00, 0x80, 0x3F,
		0x02, 'a', 'b',
		0x96, 0x00,
		0x00, 0x80,
		0x40,
		0xFF
	};
	CHECK(w.Data() == expected);
}

TEST(BytesFloatsKeepTheirExactBits)
{
	const float values[] = {
		0.0f, -0.0f, 1.5f, -123456.789f,
		(std::numeric_limits<float>::min)(),
		(std::numeric_limits<float>::denorm_min)(),
		(std::numeric_limits<float>::max)(),
		std::numeric_limits<float>::infinity(),
		-std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN()
	};

	ByteWriter w;
	for (float value : values)
	{
		w.F32(value);
	}

	ByteReader r(w.Data());
	for (float value : values)
	{
		CHECK(Bits(r.F32()) == Bits(value));
	}
	CHECK(r.Ok());
}

TEST(BytesStringsRoundTripAndCapAt255Bytes)
{
	const std::string longText(300, 'x');

	ByteWriter w;
	w.Str("");
	w.Str("hello");
	w.Str(longText);
	CHECK(w.Size() == 1 + 6 + 256);

	ByteReader r(w.Data());
	CHECK(r.Str().empty());
	CHECK(r.Str() == "hello");
	CHECK(r.Str() == std::string(255, 'x'));
	CHECK(r.Ok());
}

TEST(BytesCoordQuantizesToCentimetresAndClamps)
{
	const float inRange[] = { 0.0f, 1.234f, -1.234f, 100.0f, -250.75f, 327.67f, -327.68f };
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	ByteWriter w;
	for (float value : inRange)
	{
		w.Coord(value);
	}
	w.Coord(1000.0f);
	w.Coord(-1000.0f);
	w.Coord(inf);
	w.Coord(-inf);
	w.Coord(nan);

	ByteReader r(w.Data());
	for (float value : inRange)
	{
		CHECK(Near(r.Coord(), value, 0.005f));
	}
	CHECK(Near(r.Coord(), 327.67f, 1e-4f));
	CHECK(Near(r.Coord(), -327.68f, 1e-4f));
	CHECK(Near(r.Coord(), 327.67f, 1e-4f));
	CHECK(Near(r.Coord(), -327.68f, 1e-4f));
	CHECK(r.Coord() == 0.0f);
	CHECK(r.Ok());
}

TEST(BytesAnglesWrapAndQuantize)
{
	const float angles[] = { 0.0f, 45.0f, 90.0f, 180.0f, 270.0f, 359.0f, -90.0f, 720.0f, -725.0f, 359.999f };
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	ByteWriter w;
	for (float angle : angles)
	{
		w.Angle16(angle);
		w.Angle8(angle);
	}
	w.Angle16(nan);
	w.Angle8(inf);

	ByteReader r(w.Data());
	for (float angle : angles)
	{
		const float a16 = r.Angle16();
		const float a8 = r.Angle8();
		CHECK(a16 >= 0.0f && a16 < 360.0f);
		CHECK(a8 >= 0.0f && a8 < 360.0f);
		CHECK(AngleDistance(a16, angle) <= 360.0f / 65536.0f);
		CHECK(AngleDistance(a8, angle) <= 360.0f / 256.0f);
	}
	CHECK(r.Angle16() == 0.0f);
	CHECK(r.Angle8() == 0.0f);
	CHECK(r.Ok());
}

TEST(BytesUnit8ClampsToZeroOne)
{
	ByteWriter w;
	w.Unit8(0.0f);
	w.Unit8(1.0f);
	w.Unit8(0.5f);
	w.Unit8(-1.0f);
	w.Unit8(2.0f);
	w.Unit8(std::numeric_limits<float>::quiet_NaN());

	ByteReader r(w.Data());
	CHECK(r.Unit8() == 0.0f);
	CHECK(r.Unit8() == 1.0f);
	CHECK(Near(r.Unit8(), 0.5f, 1.0f / 255.0f));
	CHECK(r.Unit8() == 0.0f);
	CHECK(r.Unit8() == 1.0f);
	CHECK(r.Unit8() == 0.0f);
	CHECK(r.Ok());
}

TEST(BytesTruncatedReadsReturnZeroAndStayFailed)
{
	const std::vector<uint8_t> three = { 0x11, 0x22, 0x33 };

	ByteReader r32(three);
	CHECK(r32.U32() == 0);
	CHECK(!r32.Ok());
	CHECK(r32.Remaining() == 3);
	CHECK(r32.U8() == 0);
	CHECK(!r32.Ok());

	const std::vector<uint8_t> seven(7, 0xAB);
	ByteReader r64(seven);
	CHECK(r64.U64() == 0);
	CHECK(!r64.Ok());

	ByteReader rf(three);
	CHECK(Bits(rf.F32()) == 0);
	CHECK(!rf.Ok());

	const std::vector<uint8_t> shortString = { 10, 'a', 'b', 'c' };
	ByteReader rs(shortString);
	CHECK(rs.Str().empty());
	CHECK(!rs.Ok());

	ByteReader empty(nullptr, 0);
	CHECK(empty.U8() == 0);
	CHECK(empty.Coord() == 0.0f);
	CHECK(!empty.Ok());

	const std::vector<uint8_t> one = { 0x7F };
	ByteReader r16(one);
	CHECK(r16.U16() == 0);
	CHECK(!r16.Ok());

	const std::vector<uint8_t> exactString = { 3, 'a', 'b', 'c' };
	ByteReader exact(exactString);
	CHECK(exact.Str() == "abc");
	CHECK(exact.Ok());
	CHECK(exact.Remaining() == 0);
}

TEST(BytesQuantizerBoundariesMatchHeadstoneBytes)
{
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	ByteWriter w;
	w.Coord(0.125f);
	w.Coord(-0.125f);
	w.Coord(1000.0f);
	w.Coord(-1000.0f);
	w.Coord(-0.0f);
	w.Angle16(-1e-6f);
	w.Angle16(359.9999f);
	w.Angle16(359.997f);
	w.Angle8(358.6f);
	w.Angle8(359.3f);
	w.Angle8(nan);
	w.Unit8(0.5f);
	w.Unit8(inf);
	w.Unit8(-inf);

	const std::vector<uint8_t> expected = {
		0x0D, 0x00,
		0xF3, 0xFF,
		0xFF, 0x7F,
		0x00, 0x80,
		0x00, 0x00,
		0x00, 0x00,
		0x00, 0x00,
		0xFF, 0xFF,
		0xFF,
		0x00,
		0x00,
		0x80,
		0xFF,
		0x00
	};
	CHECK(w.Data() == expected);
}

TEST(BytesPeekAndClear)
{
	const std::vector<uint8_t> packet = { 7, 1, 2 };
	CHECK(PeekU8(packet) == 7);
	CHECK(PeekU8({}) == 0);

	ByteWriter w;
	w.U32(42);
	w.Clear();
	CHECK(w.Size() == 0);
	w.U8(9);
	CHECK(w.Data() == std::vector<uint8_t>{ 9 });

	const uint8_t raw[] = { 5, 6 };
	ByteReader r{ std::span<const uint8_t>(raw) };
	CHECK(r.U8() == 5);
	CHECK(r.Remaining() == 1);
	CHECK(r.Ok());
}
