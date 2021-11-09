#pragma once

#include <stddef.h>
#include <atomic>
#include <array>

namespace vst {

template<typename T, size_t N>
class LockfreeFifo {
 public:
    LockfreeFifo() = default;

    LockfreeFifo(const LockfreeFifo&) = delete;

    LockfreeFifo& operator=(const LockfreeFifo&) = delete;

    bool push(const T& data){
        return emplace(data);
    }
    template<typename... TArgs>
    bool emplace(TArgs&&... args){
        int next = (writeHead_.load(std::memory_order_relaxed) + 1) % N;
        if (next == readHead_.load(std::memory_order_relaxed)){
            return false; // FIFO is full
        }
        data_[next] = T { std::forward<TArgs>(args)... };
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

template<typename T>
class UnboundedLockfreeFifo {
    struct Node {
        Node *next_ = nullptr;
        T data_{};
    };
 public:
    UnboundedLockfreeFifo () {
        // add dummy node
        first_ = devider_ = last_ = new Node {};
    }

    UnboundedLockfreeFifo(const UnboundedLockfreeFifo&) = delete;

    UnboundedLockfreeFifo& operator=(const UnboundedLockfreeFifo&) = delete;

    ~UnboundedLockfreeFifo(){
        auto it = first_.load();
        while (it){
            auto tmp = it;
            it = it->next_;
            delete tmp;
        }
    }

    // not thread-safe!
    void reserve(size_t n){
        // check for existing empty nodes
        auto it = first_.load();
        auto end = devider_.load();
        while (it != end){
            n--;
            it = it->next_;
        }
        // add empty nodes
        while (n--){
            auto node = new Node {};
            node->next_ = first_;
            first_.store(node);
        }
    }

    void push(const T& data){
        emplace(data);
    }

    template<typename... TArgs>
    void emplace(TArgs&&... args){
        Node *node;
        for (;;){
            auto first = first_.load(std::memory_order_relaxed);
            if (first != devider_.load(std::memory_order_relaxed)){
                // try to reuse existing node
                if (first_.compare_exchange_weak(first, first->next_,
                                                 std::memory_order_acq_rel))
                {
                    first->next_ = nullptr; // !
                    node = first;
                    break;
                }
            } else {
                // create new node
                node = new Node {};
                break;
            }
        }
        node->data_ = T { std::forward<TArgs>(args)... };
        auto last = last_.load(std::memory_order_relaxed);
        last->next_ = node;
        last_.store(node, std::memory_order_release); // publish
    }

    bool pop(T& result){
        if (!empty()) {
            // use node *after* devider, because devider is always a dummy!
            auto next = devider_.load(std::memory_order_relaxed)->next_;
            result = std::move(next->data_);
            devider_.store(next, std::memory_order_release); // publish
            return true;
        } else {
            return false;
        }
    }

    bool empty() const {
        return devider_.load(std::memory_order_relaxed)
                == last_.load(std::memory_order_relaxed);
    }

    void clear(){
        devider_.store(last_);
    }
 private:
    std::atomic<Node *> first_;
    std::atomic<Node *> devider_;
    std::atomic<Node *> last_;
};

} // vst
