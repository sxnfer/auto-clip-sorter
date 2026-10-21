// Clip Atlas — scales the volume of a .wav file held in memory.
//
// Samples are read and written one byte at a time, least significant byte first, so neither the buffer's alignment
// nor the host's byte order matters. Float samples are the one exception: they go through memcpy, which is only right
// where floats are stored little-endian, so floats_are_little_endian() checks that before any float is touched.
#include "wav-volume.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace clip_atlas {

namespace {

static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4, "float must be IEEE 754 binary32");

constexpr std::size_t kRiffHeaderBytes = 12;      // "RIFF", the RIFF size, "WAVE"
constexpr std::size_t kChunkHeaderBytes = 8;      // a chunk's id and size
constexpr std::uint32_t kMinFmtBytes = 16;        // the WAVEFORMAT fields, up to wBitsPerSample
constexpr std::uint32_t kExtensibleFmtBytes = 40; // the same, through the sub-format GUID
constexpr std::uint32_t kExtensibleCbSize = 22;
constexpr std::uint32_t kTagPcm = 1;
constexpr std::uint32_t kTagFloat = 3;
constexpr std::uint32_t kTagExtensible = 0xFFFE;

// A PCM or float sub-format GUID is xxxxxxxx-0000-0010-8000-00aa00389b71, where the first two bytes of xxxxxxxx hold
// the format tag. These are the 14 bytes after those two, in file order.
constexpr std::array<std::uint8_t, 14> kSubFormatTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
							 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

// The unsigned little-endian integer in the `bytes` bytes at `p` (at most four).
std::uint32_t read_le(const std::uint8_t *p, std::size_t bytes)
{
	std::uint32_t value = 0;
	for (std::size_t i = bytes; i-- > 0;)
		value = (value << 8) | static_cast<std::uint32_t>(p[i]);
	return value;
}

void write_le(std::uint8_t *p, std::size_t bytes, std::uint32_t value)
{
	for (std::size_t i = 0; i < bytes; ++i)
		p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// True if the four bytes at `at` spell `tag`. The caller has checked that they exist.
bool has_tag(const std::vector<std::uint8_t> &file, std::size_t at, const char (&tag)[5])
{
	for (std::size_t i = 0; i < 4; ++i) {
		if (file[at + i] != static_cast<std::uint8_t>(tag[i]))
			return false;
	}
	return true;
}

// The signed value of an unsigned `bits`-bit sample (two's complement).
std::int64_t sign_extend(std::uint32_t raw, unsigned bits)
{
	const std::int64_t range = std::int64_t{1} << bits;
	const auto value = static_cast<std::int64_t>(raw);
	return value >= range / 2 ? value - range : value;
}

// The unsigned `bits`-bit pattern of a signed value that fits in `bits` bits.
std::uint32_t to_twos_complement(std::int64_t value, unsigned bits)
{
	const std::int64_t range = std::int64_t{1} << bits;
	return static_cast<std::uint32_t>(value < 0 ? value + range : value);
}

// round(sample * gain) for a signed `bits`-bit sample. Halves round away from zero, as std::round does. The clamp is a
// guard only: a gain of at most 1 can't take a sample out of range.
std::int64_t scale_signed(std::int64_t sample, double gain, unsigned bits)
{
	const double half = static_cast<double>(std::int64_t{1} << (bits - 1));
	const double scaled = std::round(static_cast<double>(sample) * gain);
	return static_cast<std::int64_t>(std::clamp(scaled, -half, half - 1.0));
}

// True where a float is stored least significant byte first, which the memcpy in scale_sample() relies on.
bool floats_are_little_endian()
{
	const float one = 1.0f;
	std::uint8_t bytes[sizeof one];
	std::memcpy(bytes, &one, sizeof one);
	return bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x80 && bytes[3] == 0x3F;
}

enum class Encoding {
	unsigned_pcm, // 8-bit, centred on 128
	signed_pcm,   // 16-, 24- or 32-bit, two's complement
	float_pcm,    // 32-bit IEEE float
};

// What the "fmt " chunk says about the samples in "data".
struct SampleFormat {
	Encoding encoding = Encoding::signed_pcm;
	unsigned bits = 16; // the container size of one sample
	std::size_t channels = 1;
	std::size_t block_align = 2; // bytes per sample frame: channels * bits / 8
};

// Reads the "fmt " chunk body at `p`, `size` bytes long, into `format`. Returns "" or the reason the samples can't be
// scaled. Only the fields up to the sample size are read, and the sub-format GUID of WAVE_FORMAT_EXTENSIBLE.
std::string read_format(const std::uint8_t *p, std::size_t size, SampleFormat &format)
{
	if (size < kMinFmtBytes)
		return "the fmt chunk is too short";
	std::uint32_t tag = read_le(p, 2);
	if (tag == kTagExtensible) {
		if (size < kExtensibleFmtBytes || read_le(p + 16, 2) < kExtensibleCbSize)
			return "the fmt chunk is too short for WAVE_FORMAT_EXTENSIBLE";
		// The sub-format GUID names the real tag. A GUID with any other tail is a format this file doesn't know.
		const bool standard_guid = std::equal(kSubFormatTail.begin(), kSubFormatTail.end(), p + 26);
		tag = read_le(p + 24, 2);
		if (!standard_guid || (tag != kTagPcm && tag != kTagFloat))
			return "unsupported WAVE_FORMAT_EXTENSIBLE sub-format";
	}
	if (tag != kTagPcm && tag != kTagFloat)
		return "unsupported format tag " + std::to_string(tag);

	const std::uint32_t channels = read_le(p + 2, 2);
	const std::uint32_t block_align = read_le(p + 12, 2);
	const std::uint32_t bits = read_le(p + 14, 2);
	SampleFormat out;
	if (tag == kTagFloat) {
		if (bits != 32)
			return "unsupported float bit depth " + std::to_string(bits);
		out.encoding = Encoding::float_pcm;
	} else {
		if (bits != 8 && bits != 16 && bits != 24 && bits != 32)
			return "unsupported PCM bit depth " + std::to_string(bits);
		out.encoding = bits == 8 ? Encoding::unsigned_pcm : Encoding::signed_pcm;
	}
	if (channels == 0)
		return "the fmt chunk has no channels";
	if (block_align != channels * (bits / 8))
		return "the block align does not match the channel count and bit depth";
	if (out.encoding == Encoding::float_pcm && !floats_are_little_endian())
		return "float samples need a little-endian host";
	out.bits = bits;
	out.channels = channels;
	out.block_align = block_align;
	format = out;
	return "";
}

// Scales the sample at `p` in place. `gain` is in [0, 1].
void scale_sample(std::uint8_t *p, const SampleFormat &format, double gain)
{
	switch (format.encoding) {
	case Encoding::unsigned_pcm:
		p[0] = static_cast<std::uint8_t>(scale_signed(static_cast<std::int64_t>(p[0]) - 128, gain, 8) + 128);
		return;
	case Encoding::signed_pcm: {
		const unsigned bits = format.bits;
		const std::size_t bytes = bits / 8;
		const std::int64_t value = sign_extend(read_le(p, bytes), bits);
		write_le(p, bytes, to_twos_complement(scale_signed(value, gain, bits), bits));
		return;
	}
	case Encoding::float_pcm: {
		float sample = 0.0f;
		std::memcpy(&sample, p, sizeof sample);
		// Zero gain gives silence even for a NaN or infinite sample, where a plain multiply would give NaN.
		sample = gain == 0.0 ? 0.0f : sample * static_cast<float>(gain);
		std::memcpy(p, &sample, sizeof sample);
		return;
	}
	}
}

} // namespace

double volume_gain(int volume_percent)
{
	const double ratio = static_cast<double>(std::clamp(volume_percent, 0, 100)) / 100.0;
	return ratio * ratio;
}

PreparedWav scale_wav_volume(const std::vector<std::uint8_t> &file, int volume_percent)
{
	PreparedWav result;
	if (file.size() > kMaxSaveSoundBytes) {
		result.error = "the file is larger than the " + std::to_string(kMaxSaveSoundBytes / (1024 * 1024)) +
			       " MB limit";
		return result;
	}
	if (file.size() < kRiffHeaderBytes) {
		result.error = "the file is too short to be a WAV file";
		return result;
	}
	if (!has_tag(file, 0, "RIFF") || !has_tag(file, 8, "WAVE")) {
		result.error = "the file is not a RIFF/WAVE file";
		return result;
	}
	// At full volume nothing is scaled, so the chunks are not parsed. A format this file can't scale still plays, and
	// a damaged fmt chunk only matters when there are samples to change.
	if (volume_percent >= 100) {
		result.bytes = file;
		return result;
	}

	SampleFormat format;
	bool have_format = false;
	bool have_data = false;
	std::size_t data_offset = 0;
	std::size_t data_size = 0;
	std::size_t pos = kRiffHeaderBytes;
	while (pos + kChunkHeaderBytes <= file.size()) {
		const std::size_t body = pos + kChunkHeaderBytes;
		const std::size_t available = file.size() - body;
		const auto declared = static_cast<std::size_t>(read_le(&file[pos + 4], 4));
		if (!have_format && has_tag(file, pos, "fmt ")) {
			if (declared > available) {
				result.error = "the fmt chunk runs past the end of the file";
				return result;
			}
			result.error = read_format(&file[body], declared, format);
			if (!result.error.empty())
				return result;
			have_format = true;
		} else if (has_tag(file, pos, "data")) {
			if (!have_format) {
				result.error = "the data chunk comes before the fmt chunk";
				return result;
			}
			// Players accept a file whose data chunk runs past its end, so the chunk is cut to what is there.
			data_offset = body;
			data_size = std::min(declared, available);
			have_data = true;
			break;
		}
		// Every other chunk, including a second "fmt ", is skipped by its size and the pad byte of an odd size.
		if (declared > available)
			break;
		pos = body + declared + declared % 2;
	}
	if (!have_data) {
		result.error = have_format ? "there is no data chunk" : "there is no fmt chunk";
		return result;
	}

	std::vector<std::uint8_t> bytes = file;
	const double gain = volume_gain(volume_percent);
	// Whole sample frames only: a trailing partial frame is left as it is.
	const std::size_t samples = data_size / format.block_align * format.channels;
	const std::size_t sample_bytes = format.bits / 8;
	for (std::size_t i = 0; i < samples; ++i)
		scale_sample(&bytes[data_offset + i * sample_bytes], format, gain);
	result.bytes = std::move(bytes);
	return result;
}

} // namespace clip_atlas
