#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cache {

struct CacheLimits {
  size_t max_entries = 100'000;
  size_t max_bytes = 256ULL * 1024 * 1024;
  size_t max_value_bytes = 1ULL * 1024 * 1024;
};

enum class PutStatus : uint8_t {
  kStored,
  kValueTooLarge,   // value exceeds limits.max_value_bytes
  kEntryTooLarge,   // entry cannot fit in max_bytes even in an empty cache
};

struct CacheSnapshot {
  size_t entries = 0;
  size_t bytes = 0;          // accounted bytes, see LruCache::memory_usage()
  size_t payload_bytes = 0;  // key + value bytes only
  uint64_t evictions = 0;
};

// Fixed-capacity LRU cache with O(1) get/put/erase.
//
// Layout:
//   std::list<Entry>                        intrusive recency order, MRU at front
//   unordered_map<string_view, iterator>    key -> position in the list
//
// The map key is a string_view into the *list node's* key string rather than a
// second copy of the key. std::list nodes are stable (splice relinks nodes, it
// never moves them), so the view stays valid for the entry's whole lifetime and
// we avoid storing every key twice.
//
// Thread safety: all public methods are internally synchronized with one
// mutex. get() is a mutating operation (it promotes to MRU), so a shared/reader
// lock would not be correct without giving up exact LRU order. No reference or
// iterator into cache state is ever handed out; get() returns a copy.
class LruCache {
 public:
  // Per-entry bookkeeping overhead charged against max_bytes, in addition to
  // key + value bytes. Rationale (libstdc++, x86-64/aarch64 LP64):
  //   list node header (prev/next)                     16 B
  //   two std::string objects inside the node          64 B
  //   unordered_map node (next ptr, hash, view, iter)  40 B
  //   amortized bucket slot                             8 B
  // Rounded up to 128 B to leave room for allocator headers. This is an
  // estimate, not an exact malloc accounting: see memory_usage().
  static constexpr size_t kEntryOverheadBytes = 128;

  explicit LruCache(CacheLimits limits);

  LruCache(const LruCache&) = delete;
  LruCache& operator=(const LruCache&) = delete;

  // Returns a copy of the value and promotes the entry to MRU.
  [[nodiscard]] std::optional<std::string> get(std::string_view key);

  // Copies into `out` (reused buffer) to avoid an allocation per hit.
  [[nodiscard]] bool get(std::string_view key, std::string& out);

  // Inserts or replaces. Replacing an existing key also promotes it to MRU.
  // Evicts from the LRU end until the new entry fits within both limits.
  PutStatus put(std::string_view key, std::string_view value);

  [[nodiscard]] bool erase(std::string_view key);

  // Membership test that does NOT change recency. Kept separate from get() on
  // purpose: a "peek" that silently promoted would be a surprising API.
  [[nodiscard]] bool contains(std::string_view key) const;

  void clear();

  [[nodiscard]] size_t size() const;

  // Accounted bytes = sum over entries of (key + value + kEntryOverheadBytes).
  // Includes: payload bytes and a fixed estimate of container overhead.
  // Excludes: unordered_map bucket array growth beyond the amortized slot,
  //           allocator fragmentation, per-connection I/O buffers, and any
  //           memory owned outside the cache.
  [[nodiscard]] size_t memory_usage() const;

  [[nodiscard]] CacheSnapshot snapshot() const;

  [[nodiscard]] CacheLimits limits() const noexcept { return limits_; }

  // Test-only introspection: keys ordered most- to least-recently used.
  [[nodiscard]] std::vector<std::string> keys_mru_to_lru() const;

 private:
  struct Entry {
    std::string key;
    std::string value;
  };

  using EntryList = std::list<Entry>;

  struct ViewHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
      return std::hash<std::string_view>{}(sv);
    }
  };

  using IndexMap =
      std::unordered_map<std::string_view, EntryList::iterator, ViewHash, std::equal_to<>>;

  static size_t entry_cost(size_t key_size, size_t value_size) noexcept {
    return key_size + value_size + kEntryOverheadBytes;
  }

  // Requires lock_ held.
  void evict_lru_locked();
  void erase_locked(IndexMap::iterator it);
  void make_room_locked(size_t incoming_cost);

  mutable std::mutex lock_;
  CacheLimits limits_;
  EntryList entries_;  // front = MRU, back = LRU
  IndexMap index_;
  size_t bytes_ = 0;
  size_t payload_bytes_ = 0;
  uint64_t evictions_ = 0;
};

}  // namespace cache
