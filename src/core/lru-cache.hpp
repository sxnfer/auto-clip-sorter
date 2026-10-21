// Clip Atlas — a small least-recently-used cache.
//
// The dock keeps one thumbnail per visible clip. Scrolling through thousands
// of clips must not keep thousands of images in memory, so the cache holds a
// fixed number of entries and forgets the one used longest ago.
//
// Header-only, pure C++17. Not thread-safe: the dock uses it on the UI thread.
#pragma once

#include <cstddef>
#include <list>
#include <map>
#include <utility>

namespace clip_atlas {

template<typename Key, typename Value> class LruCache {
public:
	// A capacity of 0 keeps nothing.
	explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

	// The value for `key`, marked as most recently used; nullptr if absent. The
	// pointer is valid until the next put(), erase() or clear().
	Value *get(const Key &key)
	{
		auto it = index_.find(key);
		if (it == index_.end())
			return nullptr;
		entries_.splice(entries_.begin(), entries_, it->second);
		return &it->second->second;
	}

	// Inserts or replaces `key` as the most recently used entry, then forgets the
	// least recently used entries beyond the capacity.
	void put(const Key &key, Value value)
	{
		auto it = index_.find(key);
		if (it != index_.end()) {
			it->second->second = std::move(value);
			entries_.splice(entries_.begin(), entries_, it->second);
		} else {
			entries_.emplace_front(key, std::move(value));
			index_.emplace(key, entries_.begin());
		}
		while (entries_.size() > capacity_) {
			index_.erase(entries_.back().first);
			entries_.pop_back();
		}
	}

	// True if `key` was present.
	bool erase(const Key &key)
	{
		auto it = index_.find(key);
		if (it == index_.end())
			return false;
		entries_.erase(it->second);
		index_.erase(it);
		return true;
	}

	void clear()
	{
		index_.clear();
		entries_.clear();
	}

	bool contains(const Key &key) const { return index_.count(key) != 0; }
	std::size_t size() const { return entries_.size(); }
	std::size_t capacity() const { return capacity_; }

private:
	using Entry = std::pair<Key, Value>;

	std::size_t capacity_;
	std::list<Entry> entries_; // most recently used first
	std::map<Key, typename std::list<Entry>::iterator> index_;
};

} // namespace clip_atlas
