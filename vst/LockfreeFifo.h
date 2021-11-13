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
struct Node {
    template<typename... U>
    Node(U&&... args)
        : next_(nullptr), data_(std::forward<U>(args)...) {}
    Node * next_;
    T data_;
};

template<typename T, typename Alloc = std::allocator<T>>
class UnboundedMPSCQueue : protected Alloc::template rebind<Node<T>>::other {
    typedef typename Alloc::template rebind<Node<T>>::other Base;
 public:
    UnboundedMPSCQueue(const Alloc& alloc = Alloc {}) : Base(alloc) {
        // add dummy node
        first_ = devider_ = last_ = &dummy_;
    }

    UnboundedMPSCQueue(const UnboundedMPSCQueue&) = delete;

    UnboundedMPSCQueue& operator=(const UnboundedMPSCQueue&) = delete;

    ~UnboundedMPSCQueue(){
        auto it = first_.load();
        while (it){
            auto tmp = it;
            it = it->next_;
            if (tmp != &dummy_) {
                tmp->~Node<T>();
                Base::deallocate(tmp, 1);
            }
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
            auto node = Base::allocate(1);
            new (node) Node<T>();
            node->next_ = first_;
            first_.store(node);
        }
    }

    void push(const T& data){
        emplace(data);
    }

    template<typename... TArgs>
    void emplace(TArgs&&... args){
        auto n = getNode();
        n->data_ = T{std::forward<TArgs>(args)...};
        pushNode(n);
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

    // not thread-safe!
    template<typename Func>
    void forEach(Func&& fn) {
        auto it = devider_.load(std::memory_order_relaxed)->next_;
        while (it) {
            fn(it->data_);
            it = it->next_;
        }
    }
 private:
    std::atomic<Node<T> *> first_;
    std::atomic<Node<T> *> devider_;
    std::atomic<Node<T> *> last_;
    std::atomic<int32_t> lock_{0};
    Node<T> dummy_; // optimization

    Node<T>* getNode() {
        for (;;){
            auto first = first_.load(std::memory_order_relaxed);
            if (first != devider_.load(std::memory_order_relaxed)){
                // try to reuse existing node
                if (first_.compare_exchange_weak(first, first->next_,
                                                 std::memory_order_acq_rel))
                {
                    first->next_ = nullptr; // !
                    return first;
                }
            } else {
                // make new node
                auto n = Base::allocate(1);
                new (n) Node<T>();
                return n;
            }
        }
    }

    void pushNode(Node<T>* n){
        while (lock_.exchange(1, std::memory_order_acquire)) ; // lock
        auto last = last_.load(std::memory_order_relaxed);
        last->next_ = n;
        last_.store(n, std::memory_order_release); // publish
        lock_.store(0, std::memory_order_release); // unlock
    }
};

} // vst
