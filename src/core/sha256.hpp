// Clip Atlas — SHA-256 (FIPS 180-4), for checking a downloaded installer against the digest GitHub publishes for it.
//
// The download is hashed piece by piece as it arrives, so the file is never read twice and never held in memory whole.
// The standard library has no hash of this kind, and Windows' own (BCrypt) would make this file untestable on Linux.
//
// Pure C++17 with no OBS dependency.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace clip_atlas {

class Sha256 {
public:
	Sha256();

	// Adds `size` bytes. Any split of the same bytes across calls gives the same digest.
	void update(const void *data, std::size_t size);
	// The digest of everything added so far, as 64 lowercase hex digits. The object is spent afterwards: call reset()
	// before hashing anything else.
	std::string hex_digest();
	// Back to the state of a new object.
	void reset();

private:
	void compress(const std::uint8_t *block);

	std::array<std::uint32_t, 8> state_{};
	std::array<std::uint8_t, 64> block_{};
	std::size_t block_size_ = 0;    // bytes waiting in block_
	std::uint64_t total_bytes_ = 0; // bytes added since reset()
};

// The digest of `bytes`, as 64 lowercase hex digits.
std::string sha256_hex(const std::string &bytes);

} // namespace clip_atlas
