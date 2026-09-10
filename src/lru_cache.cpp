#include "cache/lru_cache.h"

#include <algorithm>
#include <utility>

namespace cache {

LruCache::LruCache(CacheLimits limits) : limits_(limits) {}

bool LruCache::get(std::string_view key, std::string& out) {
  const std::lock_guard<std::mutex> guard(lock_);
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return false;
  }
  // Promote: splice moves the node itself, so the map's string_view key and
  // the stored iterator both stay valid.
  entries_.splice(entries_.begin(), entries_, it->second);
  out.assign(it->second->value);
  return true;
}

std::optional<std::string> LruCache::get(std::string_view key) {
  std::string value;
  if (!get(key, value)) {
    return std::nullopt;
  }
  return value;
}

PutStatus LruCache::put(std::string_view key, std::string_view value) {
  if (value.size() > limits_.max_value_bytes) {
    return PutStatus::kValueTooLarge;
  }
  const size_t cost = entry_cost(key.size(), value.size());
  if (cost > limits_.max_bytes || limits_.max_entries == 0) {
    // Cannot ever fit, even after evicting everything. Reject instead of
    // emptying the cache for an entry we would immediately have to drop.
    return PutStatus::kEntryTooLarge;
  }

  const std::lock_guard<std::mutex> guard(lock_);

  // Replacing a key is modelled as erase + insert: it keeps the accounting in
  // one place and guarantees the replaced entry is not an eviction candidate
  // for its own insertion.
  if (const auto it = index_.find(key); it != index_.end()) {
    erase_locked(it);
  }

  make_room_locked(cost);

  entries_.emplace_front(Entry{std::string(key), std::string(value)});
  const auto node = entries_.begin();
  // The map key views the list node's key string, which now owns the bytes.
  index_.emplace(std::string_view(node->key), node);
  bytes_ += cost;
  payload_bytes_ += key.size() + value.size();
  return PutStatus::kStored;
}

bool LruCache::erase(std::string_view key) {
  const std::lock_guard<std::mutex> guard(lock_);
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return false;
  }
  erase_locked(it);
  return true;
}

bool LruCache::contains(std::string_view key) const {
  const std::lock_guard<std::mutex> guard(lock_);
  return index_.find(key) != index_.end();
}

void LruCache::clear() {
  const std::lock_guard<std::mutex> guard(lock_);
  index_.clear();
  entries_.clear();
  bytes_ = 0;
  payload_bytes_ = 0;
}

size_t LruCache::size() const {
  const std::lock_guard<std::mutex> guard(lock_);
  return index_.size();
}

size_t LruCache::memory_usage() const {
  const std::lock_guard<std::mutex> guard(lock_);
  return bytes_;
}

CacheSnapshot LruCache::snapshot() const {
  const std::lock_guard<std::mutex> guard(lock_);
  return CacheSnapshot{index_.size(), bytes_, payload_bytes_, evictions_};
}

std::vector<std::string> LruCache::keys_mru_to_lru() const {
  const std::lock_guard<std::mutex> guard(lock_);
  std::vector<std::string> keys;
  keys.reserve(entries_.size());
  for (const Entry& entry : entries_) {
    keys.push_back(entry.key);
  }
  return keys;
}

void LruCache::erase_locked(IndexMap::iterator it) {
  const auto node = it->second;
  const size_t payload = node->key.size() + node->value.size();
  bytes_ -= entry_cost(node->key.size(), node->value.size());
  payload_bytes_ -= payload;
  // Erase from the index first: its key is a view into the node we free next.
  index_.erase(it);
  entries_.erase(node);
}

void LruCache::evict_lru_locked() {
  const auto lru = std::prev(entries_.end());
  const auto it = index_.find(std::string_view(lru->key));
  erase_locked(it);
  ++evictions_;
}

void LruCache::make_room_locked(size_t incoming_cost) {
  while (!entries_.empty() &&
         (index_.size() + 1 > limits_.max_entries || bytes_ + incoming_cost > limits_.max_bytes)) {
    evict_lru_locked();
  }
}

}  // namespace cache
