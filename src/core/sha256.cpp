#include "sha256.hpp"

#include <algorithm>
#include <cstring>

namespace clip_atlas {

namespace {

// The first 32 bits of the fractional parts of the cube roots of the first 64 primes (FIPS 180-4, 4.2.2).
constexpr std::uint32_t kRoundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

// The first 32 bits of the fractional parts of the square roots of the first 8 primes (FIPS 180-4, 5.3.3).
constexpr std::uint32_t kInitialState[8] = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

std::uint32_t rotate_right(std::uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

// Read and written big-endian whatever the machine's byte order, as the standard defines the message and digest.
std::uint32_t load_be32(const std::uint8_t *p)
{
	return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
	       (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

} // namespace

Sha256::Sha256()
{
	reset();
}

void Sha256::reset()
{
	std::memcpy(state_.data(), kInitialState, sizeof(kInitialState));
	block_size_ = 0;
	total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t *block)
{
	std::uint32_t w[64];
	for (int i = 0; i < 16; ++i)
		w[i] = load_be32(block + 4 * i);
	for (int i = 16; i < 64; ++i) {
		const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
	std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
	for (int i = 0; i < 64; ++i) {
		const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
		const std::uint32_t choose = (e & f) ^ (~e & g);
		const std::uint32_t t1 = h + s1 + choose + kRoundConstants[i] + w[i];
		const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
		const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		const std::uint32_t t2 = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	state_[0] += a;
	state_[1] += b;
	state_[2] += c;
	state_[3] += d;
	state_[4] += e;
	state_[5] += f;
	state_[6] += g;
	state_[7] += h;
}

void Sha256::update(const void *data, std::size_t size)
{
	const auto *bytes = static_cast<const std::uint8_t *>(data);
	total_bytes_ += size;
	while (size > 0) {
		const std::size_t take = std::min(size, block_.size() - block_size_);
		std::memcpy(block_.data() + block_size_, bytes, take);
		block_size_ += take;
		bytes += take;
		size -= take;
		if (block_size_ == block_.size()) {
			compress(block_.data());
			block_size_ = 0;
		}
	}
}

std::string Sha256::hex_digest()
{
	// The length is taken before the padding is added, because update() counts the padding too.
	const std::uint64_t bit_length = total_bytes_ * 8;
	const std::uint8_t one_bit = 0x80;
	update(&one_bit, 1);
	// Zeros until 8 bytes are left in the block for the length.
	const std::uint8_t zero = 0;
	while (block_size_ != block_.size() - 8)
		update(&zero, 1);
	std::uint8_t length_bytes[8];
	for (int i = 0; i < 8; ++i)
		length_bytes[i] = static_cast<std::uint8_t>(bit_length >> (56 - 8 * i));
	update(length_bytes, sizeof(length_bytes));

	static constexpr char kHex[] = "0123456789abcdef";
	std::string hex;
	hex.reserve(64);
	for (const std::uint32_t word : state_) {
		for (int shift = 28; shift >= 0; shift -= 4)
			hex.push_back(kHex[(word >> shift) & 0xf]);
	}
	return hex;
}

std::string sha256_hex(const std::string &bytes)
{
	Sha256 hash;
	hash.update(bytes.data(), bytes.size());
	return hash.hex_digest();
}

} // namespace clip_atlas
