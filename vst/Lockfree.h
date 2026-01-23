#pragma once

#include "Sync.h"

#include <stddef.h>
#include <atomic>
#include <array>
#include <memory>

namespace vst {

template<typename T, size_t N>
class LockfreeFifo {
 public:
    LockfreeFifo() = default;

    LockfreeFifo(const LockfreeFifo&) = delete;
    LockfreeFifo(LockfreeFifo&&) = delete;

    bool push(const T& data){
        return emplace(data);
    }

    template<typename... TArgs>
    bool emplace(TArgs&&... args) {
        size_t pos = writeHead_.load(std::memory_order_relaxed);
        int next = (pos + 1) % N;
        if (next == readHead_.load(std::memory_order_acquire)){
            return false; // FIFO is full
        }
        data_[pos] = T { std::forward<TArgs>(args)... };
        writeHead_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& data) {
        size_t pos = readHead_.load(std::memory_order_relaxed);
        if (pos == writeHead_.load(std::memory_order_acquire)) {
            return false; // FIFO is empty
        }
        data = data_[pos];
        size_t next = (pos + 1) % N;
        readHead_.store(next, std::memory_order_release);
        return true;
    }

    void clear() {
        readHead_.store(writeHead_.load());
    }

    // called by the writer
    bool full() const {
        size_t readPos = readHead_.load(std::memory_order_acquire);
        size_t writePos = writeHead_.load(std::memory_order_relaxed);
        int nextPos = (writePos + 1) % N;
        return nextPos == readPos;
    }

    // called by the reader
    bool empty() const {
        size_t readPos = readHead_.load(std::memory_order_relaxed);
        size_t writePos = writeHead_.load(std::memory_order_acquire);
        return readHead_ == writePos;
    }

    size_t capacity() const { return N; }

    // raw data
    size_t readPos() const { return readHead_.load(std::memory_order_relaxed); }

    size_t writePos() const { return writeHead_.load(std::memory_order_relaxed); }

    T* data() { return data_.data(); }

    const T* data() const { return data_.data(); }

private:
    std::atomic<size_t> readHead_{0};
    std::atomic<size_t> writeHead_{0};
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

// A special MPSC queue implementation that can be safely created in a RT context.
// Its main use case is to send events from one or more NRT threads to a RT thread
// or from one or more RT threads to a NRT thread. (In the latter case, a sufficient
// number of nodes must be reserved to make it RT-safe.)
//
// The required dummy node is a class member and therefore doesn't have to be allocated
// dynamically in the constructor. As a consequence, we need to be extra careful when
// freeing the nodes in the destructor (we must not delete the dummy node!)
//
// Multiple producers are synchronized with a simple spin lock. This is fine because
// the producer threads are either NRT threads or RT threads, but never both. In the
// unlikely case that a NRT thread is preempted while holding the spinlock, the worst
// thing that can happen is that another NRT thread is burning CPU cycles waiting on
// the lock. RT threads, on the other hand, shouldn't be preempted in the first place.
//
// NB: the free list *could* be atomic, but we would need to be extra careful to avoid
// the ABA problem. (During a CAS loop the current node could be popped and pushed again,
// so that the CAS would succeed even though the object has changed.)
template<typename T, typename Alloc = std::allocator<T>>
class UnboundedMPSCQueue : protected std::allocator_traits<Alloc>::template rebind_alloc<Node<T>> {
    typedef typename std::allocator_traits<Alloc>::template rebind_alloc<Node<T>> Base;
 public:
    UnboundedMPSCQueue(const Alloc& alloc = Alloc {}) : Base(alloc) {
        // add dummy node
        first_ = devider_ = last_ = &dummy_;
    }

    UnboundedMPSCQueue(const UnboundedMPSCQueue&) = delete;

    UnboundedMPSCQueue& operator=(const UnboundedMPSCQueue&) = delete;

    ~UnboundedMPSCQueue(){
        if (needRelease()) {
            freeMemory();
        }
    }

    // not thread-safe!
    void reserve(size_t n){
        // check for existing empty nodes
        auto it = first_;
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
            first_ = node;
        }
    }

    void push(const T& data){
        emplace(data);
    }

    template<typename... TArgs>
    void emplace(TArgs&&... args){
        Node<T>* node = nullptr;
        {
            // try to reuse existing node
            std::lock_guard lock(lock_);
            if (first_ != devider_.load(std::memory_order_acquire)) {
                node = first_;
                first_ = first_->next_;
                node->next_ = nullptr; // !
            }
        }
        if (!node) {
            // allocate new node
            node = Base::allocate(1);
            new (node) Node<T>();
        }
        node->data_ = T{std::forward<TArgs>(args)...};
        // push node
        std::lock_guard lock(lock_);
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
                == last_.load(std::memory_order_acquire);
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

    void release() {
        freeMemory();
        first_ = devider_ = last_ = &dummy_;
        dummy_.next_ = nullptr; // !
    }

    bool needRelease() const {
        return first_ != last_.load(std::memory_order_relaxed);
    }

 private:
    Node<T>* first_;
    std::atomic<Node<T> *> devider_;
    std::atomic<Node<T> *> last_;
    SpinLock lock_;
    Node<T> dummy_; // optimization

    void freeMemory() {
        // only frees memory, doesn't reset pointers!
        auto it = first_;
        while (it){
            auto next = it->next_;
            if (it != &dummy_) {
                it->~Node<T>();
                Base::deallocate(it, 1);
            }
            it = next;
        }
    }
};

} // vst
