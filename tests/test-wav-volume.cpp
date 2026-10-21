#include "wav-volume.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

using namespace clip_atlas;

namespace {

using Bytes = std::vector<std::uint8_t>;

void put_u16(Bytes &out, unsigned value)
{
	out.push_back(static_cast<std::uint8_t>(value & 0xFF));
	out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(Bytes &out, std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
}

void put_tag(Bytes &out, const char *tag)
{
	for (std::size_t i = 0; i < 4; ++i)
		out.push_back(static_cast<std::uint8_t>(tag[i]));
}

void append(Bytes &out, const Bytes &more)
{
	out.insert(out.end(), more.begin(), more.end());
}

// A chunk: its id, size, body, and a pad byte after an odd-sized body.
Bytes chunk(const char *id, const Bytes &body)
{
	Bytes out;
	put_tag(out, id);
	put_u32(out, static_cast<std::uint32_t>(body.size()));
	append(out, body);
	if (body.size() % 2 != 0)
		out.push_back(0);
	return out;
}

// A RIFF/WAVE image holding `chunks` in order, with its RIFF size written correctly.
Bytes riff(std::initializer_list<Bytes> chunks)
{
	Bytes body;
	put_tag(body, "WAVE");
	for (const Bytes &c : chunks)
		append(body, c);
	Bytes out;
	put_tag(out, "RIFF");
	put_u32(out, static_cast<std::uint32_t>(body.size()));
	append(out, body);
	return out;
}

// A 16-byte fmt body (WAVEFORMAT) with the given tag, channels, block align and bit depth.
Bytes fmt_body(unsigned tag, unsigned channels, unsigned block_align, unsigned bits)
{
	Bytes out;
	put_u16(out, tag);
	put_u16(out, channels);
	put_u32(out, 44100);
	put_u32(out, 44100 * block_align);
	put_u16(out, block_align);
	put_u16(out, bits);
	return out;
}

// The correct 16-byte fmt body for PCM with `channels` and `bits`.
Bytes pcm_fmt(unsigned channels, unsigned bits)
{
	return fmt_body(1, channels, channels * bits / 8, bits);
}

// A 40-byte WAVE_FORMAT_EXTENSIBLE fmt body whose sub-format GUID is the standard one for `sub_format` (1 PCM,
// 3 float).
Bytes extensible_fmt(unsigned channels, unsigned bits, unsigned sub_format)
{
	Bytes out = fmt_body(0xFFFE, channels, channels * bits / 8, bits);
	put_u16(out, 22);         // cbSize
	put_u16(out, bits);       // wValidBitsPerSample
	put_u32(out, 0x4);        // dwChannelMask
	put_u16(out, sub_format); // the first two bytes of the GUID hold the format tag
	const Bytes tail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
	append(out, tail);
	return out;
}

// A RIFF/WAVE file with one fmt chunk and one data chunk.
Bytes make_wav(const Bytes &fmt, const Bytes &data)
{
	return riff({chunk("fmt ", fmt), chunk("data", data)});
}

// 16-bit little-endian samples.
Bytes samples16(std::initializer_list<std::int64_t> values)
{
	Bytes out;
	for (std::int64_t v : values)
		put_u16(out, static_cast<std::uint16_t>(v));
	return out;
}

// 24-bit little-endian samples, the low three bytes of each value.
Bytes samples24(std::initializer_list<std::int64_t> values)
{
	Bytes out;
	for (std::int64_t v : values) {
		const auto u = static_cast<std::uint32_t>(v);
		for (unsigned shift = 0; shift < 24; shift += 8)
			out.push_back(static_cast<std::uint8_t>((u >> shift) & 0xFF));
	}
	return out;
}

// 32-bit little-endian samples.
Bytes samples32(std::initializer_list<std::int64_t> values)
{
	Bytes out;
	for (std::int64_t v : values)
		put_u32(out, static_cast<std::uint32_t>(v));
	return out;
}

// Float32 samples, by their IEEE bit pattern.
Bytes floats(std::initializer_list<float> values)
{
	Bytes out;
	for (float f : values) {
		std::uint32_t bits = 0;
		std::memcpy(&bits, &f, sizeof bits);
		put_u32(out, bits);
	}
	return out;
}

// Catch2 prints a byte vector as text, and its text wrapping can assert on some byte values, which aborts the run when
// a comparison fails. Returning a bool keeps a failure to a plain "false" that names the call.
bool same_bytes(const PreparedWav &actual, const Bytes &expected)
{
	return actual.bytes == expected;
}

} // namespace

TEST_CASE("volume_gain squares the volume and clamps it to 0-100")
{
	CHECK(volume_gain(0) == 0.0);
	CHECK(volume_gain(50) == 0.25);
	CHECK(volume_gain(100) == 1.0);
	CHECK(std::fabs(volume_gain(80) - 0.64) < 1e-12);
	CHECK(volume_gain(-20) == 0.0);
	CHECK(volume_gain(150) == 1.0);
}

TEST_CASE("16-bit PCM is scaled by the gain and rounded, with halves away from zero")
{
	const Bytes input = make_wav(pcm_fmt(1, 16), samples16({1000, -1000, 3, -3, 2, -2, 32767, -32768}));
	const Bytes expected = make_wav(pcm_fmt(1, 16), samples16({250, -250, 1, -1, 1, -1, 8192, -8192}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("16-bit PCM at 99 % keeps the extremes in range")
{
	// 32768 * 0.9801 = 32115.9168 and 32767 * 0.9801 = 32114.9367
	const Bytes input = make_wav(pcm_fmt(1, 16), samples16({-32768, 32767}));
	const Bytes expected = make_wav(pcm_fmt(1, 16), samples16({-32116, 32115}));
	CHECK(same_bytes(scale_wav_volume(input, 99), expected));
}

TEST_CASE("80 % uses the squared gain")
{
	const Bytes input = make_wav(pcm_fmt(1, 16), samples16({1000, -1000, 32767}));
	const Bytes expected = make_wav(pcm_fmt(1, 16), samples16({640, -640, 20971}));
	CHECK(same_bytes(scale_wav_volume(input, 80), expected));
}

TEST_CASE("100 % returns the file unchanged, even for a format that can't be scaled")
{
	const Bytes adpcm = make_wav(fmt_body(2, 1, 512, 4), Bytes{1, 2, 3, 4, 5, 6, 7, 8});
	CHECK(scale_wav_volume(adpcm, 100).error.empty());
	CHECK(same_bytes(scale_wav_volume(adpcm, 100), adpcm));
	CHECK(same_bytes(scale_wav_volume(adpcm, 150), adpcm));

	const Bytes pcm16 = make_wav(pcm_fmt(1, 16), samples16({1000, -1000}));
	CHECK(same_bytes(scale_wav_volume(pcm16, 100), pcm16));
}

TEST_CASE("zero volume gives silence in every sample format")
{
	const Bytes loud16 = make_wav(pcm_fmt(1, 16), samples16({1000, -1000, -32768, 32767}));
	const Bytes silent16 = make_wav(pcm_fmt(1, 16), samples16({0, 0, 0, 0}));
	CHECK(same_bytes(scale_wav_volume(loud16, 0), silent16));
	CHECK(same_bytes(scale_wav_volume(loud16, -5), silent16));

	// Unsigned 8-bit silence is 128, not 0.
	const Bytes loud8 = make_wav(pcm_fmt(1, 8), Bytes{0, 1, 200, 255});
	CHECK(same_bytes(scale_wav_volume(loud8, 0), make_wav(pcm_fmt(1, 8), Bytes{128, 128, 128, 128})));

	const Bytes loud24 = make_wav(pcm_fmt(1, 24), samples24({8388607, -8388608, 1}));
	CHECK(same_bytes(scale_wav_volume(loud24, 0), make_wav(pcm_fmt(1, 24), samples24({0, 0, 0}))));

	const Bytes loud32 = make_wav(pcm_fmt(1, 32), samples32({2147483647, -2147483648}));
	CHECK(same_bytes(scale_wav_volume(loud32, 0), make_wav(pcm_fmt(1, 32), samples32({0, 0}))));

	// A NaN or infinite float would stay NaN under a plain multiply by zero.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const Bytes loud_float = make_wav(fmt_body(3, 1, 4, 32), floats({1.0f, -3.5f, nan, inf}));
	CHECK(same_bytes(scale_wav_volume(loud_float, 0),
			 make_wav(fmt_body(3, 1, 4, 32), floats({0.0f, 0.0f, 0.0f, 0.0f}))));
}

TEST_CASE("8-bit PCM is unsigned and centred on 128")
{
	const Bytes input = make_wav(pcm_fmt(1, 8), Bytes{255, 0, 128, 129, 130, 126});
	// 127 -> 31.75, -128 -> -32, 0 -> 0, 1 -> 0.25, 2 -> 0.5, -2 -> -0.5 (then + 128)
	const Bytes expected = make_wav(pcm_fmt(1, 8), Bytes{160, 96, 128, 128, 129, 127});
	CHECK(same_bytes(scale_wav_volume(input, 50), expected));
}

TEST_CASE("24-bit PCM is scaled as a signed three-byte value")
{
	const Bytes input = make_wav(pcm_fmt(1, 24), samples24({8388607, -8388608, -1000, -2, 2, 256, 1}));
	// 8388607 * 0.25 = 2097151.75 rounds up to the top of the range; -8388608 is the bottom edge.
	const Bytes expected = make_wav(pcm_fmt(1, 24), samples24({2097152, -2097152, -250, -1, 1, 64, 0}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("32-bit integer PCM is scaled as a signed four-byte value")
{
	const Bytes input = make_wav(pcm_fmt(1, 32), samples32({2147483647, -2147483648, 1000, -1, 3}));
	const Bytes expected = make_wav(pcm_fmt(1, 32), samples32({536870912, -536870912, 250, 0, 1}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("float32 samples are multiplied by the gain")
{
	const Bytes input = make_wav(fmt_body(3, 1, 4, 32), floats({0.5f, -1.0f, 3.0f, 0.0f}));
	const Bytes expected = make_wav(fmt_body(3, 1, 4, 32), floats({0.125f, -0.25f, 0.75f, 0.0f}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE PCM is scaled like the tag it names")
{
	const Bytes input = make_wav(extensible_fmt(1, 16, 1), samples16({1000, -1000}));
	const Bytes expected = make_wav(extensible_fmt(1, 16, 1), samples16({250, -250}));
	CHECK(same_bytes(scale_wav_volume(input, 50), expected));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE float is scaled like the tag it names")
{
	const Bytes input = make_wav(extensible_fmt(1, 32, 3), floats({0.5f, -1.0f}));
	const Bytes expected = make_wav(extensible_fmt(1, 32, 3), floats({0.125f, -0.25f}));
	CHECK(same_bytes(scale_wav_volume(input, 50), expected));
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE with a non-standard GUID is refused below 100 % only")
{
	Bytes fmt = extensible_fmt(1, 16, 1);
	fmt[39] = 0x72; // the last byte of the GUID, 71 in the standard one
	const Bytes input = make_wav(fmt, samples16({1000}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.bytes.empty());
	CHECK(out.error == "unsupported WAVE_FORMAT_EXTENSIBLE sub-format");
	CHECK(same_bytes(scale_wav_volume(input, 100), input));

	// A GUID for a format this file can't scale (sub-format 2, ADPCM) is refused the same way.
	const Bytes adpcm = make_wav(extensible_fmt(1, 16, 2), samples16({1000}));
	CHECK(scale_wav_volume(adpcm, 50).error == "unsupported WAVE_FORMAT_EXTENSIBLE sub-format");
}

TEST_CASE("WAVE_FORMAT_EXTENSIBLE needs cbSize of at least 22")
{
	Bytes fmt = extensible_fmt(1, 16, 1);
	fmt[16] = 0;
	fmt[17] = 0;
	const PreparedWav out = scale_wav_volume(make_wav(fmt, samples16({1000})), 50);
	CHECK(out.bytes.empty());
	CHECK(out.error == "the fmt chunk is too short for WAVE_FORMAT_EXTENSIBLE");
}

TEST_CASE("a fmt chunk that is too short is an error")
{
	Bytes fmt(14, 0);
	fmt[0] = 1;
	const PreparedWav out = scale_wav_volume(make_wav(fmt, samples16({1000})), 50);
	CHECK(out.bytes.empty());
	CHECK(out.error == "the fmt chunk is too short");
}

TEST_CASE("a fmt chunk longer than the 16-byte WAVEFORMAT is fine")
{
	Bytes fmt = pcm_fmt(1, 16);
	put_u16(fmt, 0); // cbSize of an 18-byte WAVEFORMATEX
	const Bytes input = make_wav(fmt, samples16({1000, -1000}));
	const Bytes expected = make_wav(fmt, samples16({250, -250}));
	CHECK(same_bytes(scale_wav_volume(input, 50), expected));
}

TEST_CASE("an unknown chunk before fmt and odd-sized chunks with pad bytes are walked and kept")
{
	const Bytes list = chunk("LIST", Bytes{'I', 'N', 'F', 'O', 'x'});
	const Bytes junk = chunk("junk", Bytes{1, 2, 3});
	const Bytes input = riff({list, chunk("fmt ", pcm_fmt(1, 16)), chunk("data", samples16({1000, -1000})), junk});
	const Bytes expected = riff({list, chunk("fmt ", pcm_fmt(1, 16)), chunk("data", samples16({250, -250})), junk});
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("a data chunk before the fmt chunk is an error")
{
	const Bytes input = riff({chunk("data", samples16({1000})), chunk("fmt ", pcm_fmt(1, 16))});
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.bytes.empty());
	CHECK(out.error == "the data chunk comes before the fmt chunk");
}

TEST_CASE("a file with no data chunk or no fmt chunk is an error")
{
	const PreparedWav no_data = scale_wav_volume(riff({chunk("fmt ", pcm_fmt(1, 16))}), 50);
	CHECK(no_data.bytes.empty());
	CHECK(no_data.error == "there is no data chunk");

	const PreparedWav no_fmt = scale_wav_volume(riff({chunk("LIST", Bytes{1, 2})}), 50);
	CHECK(no_fmt.bytes.empty());
	CHECK(no_fmt.error == "there is no fmt chunk");
}

TEST_CASE("a fmt chunk that runs past the end of the file is an error")
{
	Bytes input;
	put_tag(input, "RIFF");
	put_u32(input, 20);
	put_tag(input, "WAVE");
	put_tag(input, "fmt ");
	put_u32(input, 16); // declares 16 bytes, but only 4 follow
	append(input, Bytes{1, 0, 1, 0});
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.bytes.empty());
	CHECK(out.error == "the fmt chunk runs past the end of the file");
}

TEST_CASE("a file that is not RIFF/WAVE is an error at every volume")
{
	Bytes riff_x = make_wav(pcm_fmt(1, 16), samples16({1000}));
	riff_x[3] = 'X'; // RIFX
	CHECK(scale_wav_volume(riff_x, 50).error == "the file is not a RIFF/WAVE file");
	CHECK(scale_wav_volume(riff_x, 100).error == "the file is not a RIFF/WAVE file");

	Bytes wavx = make_wav(pcm_fmt(1, 16), samples16({1000}));
	wavx[11] = 'X'; // WAVX
	CHECK(scale_wav_volume(wavx, 50).error == "the file is not a RIFF/WAVE file");
	CHECK(scale_wav_volume(wavx, 100).error == "the file is not a RIFF/WAVE file");
}

TEST_CASE("a truncated header is an error at every volume")
{
	const Bytes truncated{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V'};
	CHECK(scale_wav_volume(truncated, 50).error == "the file is too short to be a WAV file");
	CHECK(scale_wav_volume(truncated, 100).error == "the file is too short to be a WAV file");
	CHECK(scale_wav_volume(Bytes{}, 50).error == "the file is too short to be a WAV file");
}

TEST_CASE("a data chunk longer than the file is cut to the bytes that are there")
{
	// The header claims 1000 bytes of samples, but only four follow.
	Bytes data_header;
	put_tag(data_header, "data");
	put_u32(data_header, 1000);
	Bytes input = riff({chunk("fmt ", pcm_fmt(1, 16)), data_header});
	append(input, samples16({1000, -1000}));
	Bytes expected = riff({chunk("fmt ", pcm_fmt(1, 16)), data_header});
	append(expected, samples16({250, -250}));
	const PreparedWav out = scale_wav_volume(input, 50);
	CHECK(out.error.empty());
	CHECK(out.bytes.size() == input.size());
	CHECK(same_bytes(out, expected));
}

TEST_CASE("a trailing partial sample frame is left unchanged")
{
	// Stereo 16-bit: one whole frame, then two bytes of a second frame.
	const Bytes input = make_wav(pcm_fmt(2, 16), Bytes{0xE8, 0x03, 0x18, 0xFC, 0xE8, 0x03});
	const Bytes expected = make_wav(pcm_fmt(2, 16), Bytes{0xFA, 0x00, 0x06, 0xFF, 0xE8, 0x03});
	CHECK(same_bytes(scale_wav_volume(input, 50), expected));
}

TEST_CASE("a file larger than kMaxSaveSoundBytes is refused, and one exactly at the limit is accepted")
{
	const Bytes too_big(kMaxSaveSoundBytes + 1, 0);
	const PreparedWav refused = scale_wav_volume(too_big, 50);
	CHECK(refused.bytes.empty());
	CHECK(refused.error == "the file is larger than the 10 MB limit");
	CHECK(scale_wav_volume(too_big, 100).error == refused.error);

	// 44 bytes go to the RIFF, fmt and data headers, so the data chunk takes the rest of the limit.
	Bytes samples(kMaxSaveSoundBytes - 44, 0);
	samples[0] = 0xE8; // 1000, little-endian
	samples[1] = 0x03;
	const Bytes at_limit = make_wav(pcm_fmt(1, 16), samples);
	REQUIRE(at_limit.size() == kMaxSaveSoundBytes);
	const PreparedWav out = scale_wav_volume(at_limit, 50);
	REQUIRE(out.error.empty());
	REQUIRE(out.bytes.size() == kMaxSaveSoundBytes);
	CHECK(out.bytes[44] == 0xFA); // 250
	CHECK(out.bytes[45] == 0x00);
}

TEST_CASE("unsupported formats are refused below 100 %")
{
	CHECK(scale_wav_volume(make_wav(fmt_body(2, 1, 512, 4), Bytes{1, 2}), 50).error == "unsupported format tag 2");
	CHECK(scale_wav_volume(make_wav(fmt_body(3, 1, 8, 64), Bytes(8, 0)), 50).error ==
	      "unsupported float bit depth 64");
	CHECK(scale_wav_volume(make_wav(fmt_body(1, 1, 2, 12), Bytes{1, 2}), 50).error ==
	      "unsupported PCM bit depth 12");
	CHECK(scale_wav_volume(make_wav(fmt_body(1, 2, 3, 16), Bytes{1, 2}), 50).error ==
	      "the block align does not match the channel count and bit depth");
	CHECK(scale_wav_volume(make_wav(fmt_body(1, 0, 0, 16), Bytes{1, 2}), 50).error ==
	      "the fmt chunk has no channels");
}

TEST_CASE("an unsupported format still plays at 100 % and is refused at 99 %")
{
	const Bytes input = make_wav(fmt_body(2, 1, 512, 4), Bytes{1, 2});
	CHECK(same_bytes(scale_wav_volume(input, 100), input));
	CHECK(scale_wav_volume(input, 99).error == "unsupported format tag 2");
}
