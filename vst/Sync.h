#pragma once

#ifndef _WIN32
#include <pthread.h>
#endif

#ifdef _WIN32
  // Windows Event
  // #include "synchapi.h"
#else
  #include <pthread.h>
  #ifdef __APPLE__
    // macOS doesn't support unnamed pthread semaphores,
    // so we use Mach semaphores instead
    #include <mach/mach.h>
  #else
    // unnamed pthread semaphore
    #include <semaphore.h>
  #endif
#endif

#include <atomic>

namespace vst {

/*/////////////////// SyncCondition ///////////////////////*/

class SyncCondition {
 public:
    SyncCondition();
    ~SyncCondition();
    SyncCondition(SyncCondition&&) = delete;
    SyncCondition& operator=(SyncCondition&&) = delete;
    void set();
    void wait();
 private:
#ifdef _WIN32
    void *condition_;
    void *mutex_;
#else
    pthread_cond_t condition_;
    pthread_mutex_t mutex_;
#endif
    bool state_ = false;
};

/*////////////////// Semaphore ////////////////////////*/

class Semaphore {
 public:
    Semaphore();
    ~Semaphore();
    Semaphore(Semaphore&&) = delete;
    Semaphore& operator=(Semaphore&&) = delete;
    void post();
    void wait();
 private:
#if defined(_WIN32)
    void *sem_;
#elif defined(__APPLE__)
    semaphore_t sem_;
#else // pthreads
    sem_t sem_;
#endif
};

// thanks to https://preshing.com/20150316/semaphores-are-surprisingly-versatile/

/*/////////////////// LightSemaphore /////////////////*/

class LightSemaphore {
 public:
    void post(){
        auto old = count_.fetch_add(1, std::memory_order_release);
        if (old < 0){
            sem_.post();
        }
    }
    void wait(){
        auto old = count_.fetch_sub(1, std::memory_order_acquire);
        if (old <= 0){
            sem_.wait();
        }
    }
 private:
    Semaphore sem_;
    std::atomic<int32_t> count_{0};
};

/*////////////////// Event ///////////////////////*/

class Event {
 public:
    void set(){
        int oldcount = count_.load(std::memory_order_relaxed);
        for (;;) {
            // don't increment past 1
            // NOTE: we have to use the CAS loop even if we don't
            // increment 'oldcount', because a another thread
            // might decrement the counter concurrently!
            auto newcount = oldcount >= 0 ? 1 : oldcount + 1;
            if (count_.compare_exchange_weak(oldcount, newcount, std::memory_order_release,
                                             std::memory_order_relaxed))
                break;
        }
        if (oldcount < 0)
            sem_.post(); // release one waiting thread
    }
    void wait(){
        auto old = count_.fetch_sub(1, std::memory_order_acquire);
        if (old <= 0){
            sem_.wait();
        }
    }
 private:
    Semaphore sem_;
    std::atomic<int32_t> count_{0};
};

/*///////////////////// SpinLock /////////////////////*/

// simple spin lock

class SpinLock {
 public:
    SpinLock() = default;
    SpinLock(SpinLock&&) = delete;
    SpinLock& operator=(SpinLock&&) = delete;
    void lock(){
        // only try to modify the shared state if the lock seems to be available.
        // this should prevent unnecessary cache invalidation.
        do {
            while (locked_.load(std::memory_order_relaxed)){
                yield();
            }
        } while (locked_.exchange(true, std::memory_order_acquire));
    }
    bool try_lock(){
        return !locked_.exchange(true, std::memory_order_acquire);
    }
    void unlock(){
        locked_.store(false, std::memory_order_release);
    }
 protected:
    void yield();
    // data
    std::atomic<int32_t> locked_{false};
};


const size_t CACHELINE_SIZE = 64;

class alignas(CACHELINE_SIZE) PaddedSpinLock : public SpinLock {
 public:
    PaddedSpinLock();
    // before C++17, new() couldn't handle alignments larger than max_align_t
#if __cplusplus < 201703L
    void* operator new(size_t size);
    void* operator new[](size_t size);
    void operator delete(void*);
    void operator delete[](void*);
#endif
 private:
    // pad and align to prevent false sharing
    char pad_[CACHELINE_SIZE - sizeof(locked_)];
};

/*//////////////////////// SharedMutex //////////////////////////*/

// The std::mutex implementation on Windows is bad on both MSVC and MinGW:
// the MSVC version apparantely has some additional overhead; winpthreads (MinGW) doesn't even use the obvious
// platform primitive (SRWLOCK), they rather roll their own mutex based on atomics and Events, which is bad for our use case.
// Even on Linux and macOS, there's some overhead for things we don't need, so we use pthreads directly.
#ifdef _WIN32
class Mutex {
public:
    Mutex();
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;
    // exclusive
    void lock();
    bool try_lock();
    void unlock();
protected:
    void* lock_; // avoid including windows headers (SWRLOCK is pointer sized)
};
#else
class Mutex {
public:
    Mutex() { pthread_mutex_init(&lock_, nullptr); }
    ~Mutex() { pthread_mutex_destroy(&lock_); }
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;
    // exclusive
    void lock() { pthread_mutex_lock(&lock_); }
    bool try_lock() { return pthread_mutex_trylock(&lock_) == 0; }
    void unlock() { pthread_mutex_unlock(&lock_); }
private:
    pthread_mutex_t lock_;
};
#endif

class ScopedLock {
    Mutex& mutex_;
 public:
    ScopedLock(Mutex& mutex)
        : mutex_(mutex){ mutex_.lock(); }
    ~ScopedLock(){ mutex_.unlock(); }
    ScopedLock(ScopedLock&&) = delete;
    ScopedLock& operator=(ScopedLock&&) = delete;
};

template<typename T>
class UniqueLock {
 protected:
    T* mutex_;
    bool owns_;
 public:
    UniqueLock() : mutex_(nullptr), owns_(false){}
    UniqueLock(T& mutex)
        : mutex_(&mutex), owns_(true)
    {
        mutex_->lock();
    }
    UniqueLock(const UniqueLock&) = delete;
    UniqueLock(UniqueLock&& other)
        : mutex_(other.mutex_), owns_(other.owns_)
    {
        other.mutex_ = nullptr;
        other.owns_ = false;
    }

    ~UniqueLock(){
        if (owns_){
            mutex_->unlock();
        }
    }

    UniqueLock& operator=(const UniqueLock&) = delete;
    UniqueLock& operator=(UniqueLock&& other){
        if (owns_){
            mutex_->unlock();
        }
        mutex_ = other.mutex_;
        owns_ = other.owns_;
        other.mutex_ = nullptr;
        other.owns_ = false;
    }

    void lock(){
        mutex_->lock();
        owns_ = true;
    }

    void unlock(){
        mutex_->unlock();
        owns_ = false;
    }
};

class Lock : public UniqueLock<Mutex> {
    using UniqueLock::UniqueLock;
};

/*//////////////////////// SharedMutex ///////////////////////////*/

#ifdef _WIN32
class SharedMutex : public Mutex {
public:
    using Mutex::Mutex;
    // shared
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();
};
#else
class SharedMutex {
public:
    SharedMutex() { pthread_rwlock_init(&rwlock_, nullptr); }
    ~SharedMutex() { pthread_rwlock_destroy(&rwlock_); }
    SharedMutex(SharedMutex&&) = delete;
    SharedMutex& operator=(SharedMutex&&) = delete;
    // exclusive
    void lock() { pthread_rwlock_wrlock(&rwlock_); }
    bool try_lock() { return pthread_rwlock_trywrlock(&rwlock_) == 0; }
    void unlock() { pthread_rwlock_unlock(&rwlock_); }
    // shared
    void lock_shared() { pthread_rwlock_rdlock(&rwlock_); }
    bool try_lock_shared() { return pthread_rwlock_tryrdlock(&rwlock_) == 0; }
    void unlock_shared() { pthread_rwlock_unlock(&rwlock_); }
private:
    pthread_rwlock_t rwlock_;
};
#endif

class WriteLock : public UniqueLock<SharedMutex> {
    using UniqueLock::UniqueLock;
};

class ReadLock {
 protected:
    SharedMutex* mutex_;
    bool owns_;
 public:
    ReadLock() : mutex_(nullptr), owns_(false){}
    ReadLock(SharedMutex& mutex)
        : mutex_(&mutex), owns_(true)
    {
        mutex_->lock_shared();
    }
    ReadLock(const ReadLock&) = delete;
    ReadLock(ReadLock&& other)
        : mutex_(other.mutex_), owns_(other.owns_)
    {
        other.mutex_ = nullptr;
        other.owns_ = false;
    }

    ~ReadLock(){
        if (owns_){
            mutex_->unlock_shared();
        }
    }

    ReadLock& operator=(const ReadLock&) = delete;
    ReadLock& operator=(ReadLock&& other){
        if (owns_){
            mutex_->unlock_shared();
        }
        mutex_ = other.mutex_;
        owns_ = other.owns_;
        other.mutex_ = nullptr;
        other.owns_ = false;
        return *this;
    }

    void lock(){
        mutex_->lock_shared();
        owns_ = true;
    }

    void unlock(){
        mutex_->unlock_shared();
        owns_ = false;
    }
};


} //  vst
