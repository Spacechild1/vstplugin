#pragma once

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <vector>

namespace vst {

// Custom open addressing hashtable that supports custom view types
// for keys to avoid expensive conversions in find() method.
// A typical use case would be std::string_view for std::string keys.
// Key must be constructible from KeyView, KeyView must be convertable
// to Key and both types must be comparable with each other.
template<typename Key, typename Value, typename KeyView = Key>
class HashTable {
public:
    HashTable() { array_.resize(initialCapacity); };

    template<typename U>
    bool insert(const KeyView& key, U&& value) {
        return insert(Key{key}, std::forward<U>(value));
    }

    template<typename U>
    bool insert(Key&& key, U&& value);

    const Value* find(const KeyView& key) const;

    template<typename U>
    Value findOr(const KeyView& key, U&& value) const {
        if (auto result = find(key)) {
            return *result;
        } else {
            return std::forward<U>(value);
        }
    }

    size_t size() const {
        return count_;
    }

    void clear() {
        for (auto& e : array_) {
            e = Entry{};
        }
        count_ = 0;
    }
private:
    static constexpr size_t initialCapacity = 8;

    using HashType = uint32_t;
    static constexpr HashType flag = (HashType)1 << (sizeof(HashType) * CHAR_BIT - 1);

    static HashType makeHash(const KeyView& key) {
        HashType hash = std::hash<KeyView>{}(key);
        return hash & ~flag;
    }

    void rehash() {
        auto newSize = array_.size() * 2;
        auto oldArray = std::move(array_);
        array_ = std::vector<Entry>(newSize);
        for (auto& e : oldArray) {
            if (!e.empty()) {
                auto success = insert(std::move(e.key_), std::move(e.value_));
                assert(success);
            }
        }
    }

    struct Entry {
        Key key_{};
        HashType hash_{}; // highest bit is reserved (1 = slot is occupied)
        Value value_{};

        HashType hash() const {
            return hash_ & ~flag;
        }

        bool empty() const {
            return !(hash_ & flag);
        }
    };

    std::vector<Entry> array_;
    size_t count_ = 0;
};

template<typename Key, typename Value, typename KeyView>
template<typename U>
inline bool HashTable<Key, Value, KeyView>::insert(Key&& key, U&& value) {
    // rehash if load factor exceeds 0.5
    if (count_ >= array_.size() / 2) {
        rehash();
    }

    const auto hash = makeHash(key);
    // NB: array size is always power of two!
    const auto mask = array_.size() - 1;
    for (size_t index = hash;; ++index) {
        index = index & mask;
        if (array_[index].empty()) {
            // found free slot
            array_[index].key_ = std::move(key);
            array_[index].hash_ = hash | flag; // mark as occupied!
            array_[index].value_ = std::forward<U>(value);

            count_++;

            return true;
        } else {
            // check if key already exists!
            if (hash == array_[index].hash() && key == array_[index].key_) {
                return false;
            }
        }
    }
    // unreachable (the hashtable should always contain empty slots)
    assert(false);
    return false;
}

template<typename Key, typename Value, typename KeyView>
const Value* HashTable<Key, Value, KeyView>::find(const KeyView& key) const {
    const auto hash = makeHash(key);
    // NB: array size is always power of two!
    const auto mask = array_.size() - 1;
    for (size_t index = hash;; ++index) {
        index = index & mask;
        if (array_[index].empty()) {
            return nullptr; // hit empty slot
        } else {
            if (hash == array_[index].hash() && key == array_[index].key_) {
                return &array_[index].value_; // found match!
            }
        }
    }
    // unreachable (the hashtable should always contain empty slots)
    assert(false);
    return nullptr;
}

} // namespace vst
