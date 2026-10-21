#include "lru-cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace clip_atlas;

TEST_CASE("LruCache returns what was put and nullptr for a miss", "[lru-cache]")
{
	LruCache<std::string, int> cache(3);
	CHECK(cache.get("a") == nullptr);
	cache.put("a", 1);
	REQUIRE(cache.get("a") != nullptr);
	CHECK(*cache.get("a") == 1);
	CHECK(cache.contains("a"));
	CHECK(cache.size() == 1);
	CHECK(cache.capacity() == 3);
}

TEST_CASE("LruCache forgets the least recently used entry", "[lru-cache]")
{
	LruCache<std::string, int> cache(2);
	cache.put("a", 1);
	cache.put("b", 2);
	REQUIRE(cache.get("a") != nullptr); // a is now newer than b
	cache.put("c", 3);
	CHECK(cache.contains("a"));
	CHECK_FALSE(cache.contains("b"));
	CHECK(cache.contains("c"));
	CHECK(cache.size() == 2);
}

TEST_CASE("LruCache put replaces a value and marks it as used", "[lru-cache]")
{
	LruCache<std::string, int> cache(2);
	cache.put("a", 1);
	cache.put("b", 2);
	cache.put("a", 10);
	cache.put("c", 3);
	REQUIRE(cache.get("a") != nullptr);
	CHECK(*cache.get("a") == 10);
	CHECK_FALSE(cache.contains("b"));
	CHECK(cache.size() == 2);
}

TEST_CASE("LruCache erase and clear", "[lru-cache]")
{
	LruCache<int, std::string> cache(4);
	cache.put(1, "one");
	cache.put(2, "two");
	CHECK(cache.erase(1));
	CHECK_FALSE(cache.erase(1));
	CHECK_FALSE(cache.contains(1));
	CHECK(cache.size() == 1);
	cache.clear();
	CHECK(cache.size() == 0);
	CHECK(cache.get(2) == nullptr);
	cache.put(3, "three"); // still usable after clear
	CHECK(cache.contains(3));
}

TEST_CASE("LruCache with capacity 0 keeps nothing", "[lru-cache]")
{
	LruCache<int, int> cache(0);
	cache.put(1, 1);
	CHECK(cache.size() == 0);
	CHECK(cache.get(1) == nullptr);
}
