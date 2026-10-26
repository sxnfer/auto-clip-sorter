#include "sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

using namespace clip_atlas;

namespace {

// Bytes 0, 1, 2, ... wrapping at 251, so no run of the pattern lines up with the 64-byte block.
std::string pattern(std::size_t size)
{
	std::string bytes(size, '\0');
	for (std::size_t i = 0; i < size; ++i)
		bytes[i] = static_cast<char>(i % 251);
	return bytes;
}

} // namespace

TEST_CASE("sha256_hex matches the FIPS 180-4 examples", "[sha256]")
{
	CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(sha256_hex("The quick brown fox jumps over the lazy dog") ==
	      "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
	CHECK(sha256_hex(std::string(1000000, 'a')) ==
	      "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256_hex is right on each side of the padding boundaries", "[sha256]")
{
	// 55 bytes still fit the length in the same block; 56 need a second one. 64 and 120 fill blocks exactly.
	// Expected values from Python's hashlib.
	CHECK(sha256_hex(pattern(55)) == "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59");
	CHECK(sha256_hex(pattern(56)) == "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562");
	CHECK(sha256_hex(pattern(63)) == "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488");
	CHECK(sha256_hex(pattern(64)) == "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108");
	CHECK(sha256_hex(pattern(65)) == "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781");
	CHECK(sha256_hex(pattern(119)) == "da18797ed7c3a777f0847f429724a2d8cd5138e6ed2895c3fa1a6d39d18f7ec6");
	CHECK(sha256_hex(pattern(120)) == "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c");
}

TEST_CASE("Sha256 gives the same digest however the bytes are split", "[sha256]")
{
	const std::string bytes = pattern(1000);
	const std::string expected = sha256_hex(bytes);
	for (const std::size_t piece :
	     {std::size_t{1}, std::size_t{7}, std::size_t{63}, std::size_t{64}, std::size_t{65}, std::size_t{999}}) {
		Sha256 hash;
		for (std::size_t offset = 0; offset < bytes.size(); offset += piece)
			hash.update(bytes.data() + offset, std::min(piece, bytes.size() - offset));
		CHECK(hash.hex_digest() == expected);
	}
}

TEST_CASE("Sha256 ignores empty updates and starts over after reset", "[sha256]")
{
	Sha256 hash;
	hash.update(nullptr, 0);
	hash.update("ab", 2);
	hash.update(nullptr, 0);
	hash.update("c", 1);
	CHECK(hash.hex_digest() == sha256_hex("abc"));

	hash.reset();
	CHECK(hash.hex_digest() == sha256_hex(""));
}
