#pragma once

#include <atomic>
#include <array>

namespace vst {

template<typename T, size_t N>
class LockfreeFifo {
 public:
    template<typename... TArgs>
    bool emplace(TArgs&&... args){
        return push(T{std::forward<TArgs>(args)...});
    }
    bool push(const T& data){
        int next = (writeHead_.load(std::memory_order_relaxed) + 1) % N;
        if (next == readHead_.load(std::memory_order_relaxed)){
            return false; // FIFO is full
        }
        data_[next] = data;
        writeHead_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& data){
        int pos = readHead_.load(std::memory_order_relaxed);
        if (pos == writeHead_.load()){
            return false; // FIFO is empty
        }
        int next = (pos + 1) % N;
        data = data_[next];
        readHead_.store(next, std::memory_order_release);
        return true;
    }
    void clear() {
        readHead_.store(writeHead_.load());
    }
    bool empty() const {
        return readHead_.load(std::memory_order_relaxed) == writeHead_.load(std::memory_order_relaxed);
    }
    size_t capacity() const { return N; }
    // raw data
    int readPos() const { return readHead_.load(std::memory_order_relaxed); }
    int writePos() const { return writeHead_.load(std::memory_order_relaxed); }
    T * data() { return data_.data(); }
    const T* data() const { return data_.data(); }
 private:
    std::atomic<int> readHead_{0};
    std::atomic<int> writeHead_{0};
    std::array<T, N> data_;
};

} // vst
