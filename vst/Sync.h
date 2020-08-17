#pragma once

// for SharedLock
#ifndef _WIN32
#include <pthread.h>
#endif

// for SpinLock
#include <atomic>
// Intel
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
  #define CPU_INTEL
  #include <immintrin.h>
#endif
// ARM
#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__)
  #define CPU_ARM
  #include <intrinsics.h>
#endif

// for Event
#ifndef USE_PLATFORM_EVENT
#define USE_PLATFORM_EVENT 1
#endif

#if USE_PLATFORM_EVENT
#if defined(_WIN32)
  // Windows Event
  // #include "synchapi.h"
#elif defined(__APPLE__)
  // macOS doesn't support unnamed pthread semaphores,
  // so we use GCD semaphores instead
  #include <dispatch/dispatch.h>
#else
  // unnamed pthread semaphore
  #include <semaphore.h>
#endif
#else // C++11 condition_variable
  #include <condition_variable>
  #include <mutex>
#endif

namespace vst {

/*////////////////// Event ////////////////////////*/

class Event {
 public:
    Event();
    ~Event();
    void signal();
    void wait();
 private:
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    void * event_; // avoid including windows headers
#elif defined(__APPLE__)
    dispatch_semaphore_t sem_;
#else // pthreads
    sem_t sem_;
#endif
#else // USE_PLATFORM_EVENT
    std::condition_variable condition_;
    std::mutex mutex_;
    bool state_ = false;
#endif
};

/*///////////////////// SpinLock /////////////////////*/

// simple spin lock
static const size_t CACHELINE_SIZE = 64;

class alignas(CACHELINE_SIZE) SpinLock {
 public:
    SpinLock();
    void lock();
    bool try_lock();
    void unlock();
 private:
    // pad and align to prevent false sharing
    std::atomic_bool locked_{false};
    char pad_[CACHELINE_SIZE - sizeof(locked_)];
};

/*//////////////////////// SharedMutex //////////////////////////*/

// The std::mutex implementation on Windows is bad on both MSVC and MinGW:
// the MSVC version apparantely has some additional overhead; winpthreads (MinGW) doesn't even use the obvious
// platform primitive (SRWLOCK), they rather roll their own mutex based on atomics and Events, which is bad for our use case.
//
// Older OSX versions (OSX 10.11 and below) don't have std:shared_mutex...
//
// Even on Linux, there's some overhead for things we don't need, so we use pthreads directly.

#ifdef _WIN32
class SharedMutex {
public:
    SharedMutex();
    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator==(const SharedMutex&) = delete;
    // exclusive
    void lock();
    bool try_lock();
    void unlock();
    // shared
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();
private:
    void* rwlock_; // avoid including windows headers (SWRLOCK is pointer sized)
};
#else
class SharedMutex {
public:
    SharedMutex() { pthread_rwlock_init(&rwlock_, nullptr); }
    ~SharedMutex() { pthread_rwlock_destroy(&rwlock_); }
    SharedMutex(const SharedMutex&) = delete;
    SharedMutex& operator==(const SharedMutex&) = delete;
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

class LockGuard {
    SharedMutex& mutex_;
 public:
    LockGuard(SharedMutex& mutex)
        : mutex_(mutex){ mutex_.lock(); }
    ~LockGuard(){ mutex_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
};

template<bool write>
class BaseLock {
 protected:
    SharedMutex* mutex_;
    bool owns_;
 public:
    BaseLock() : mutex_(nullptr), owns_(false){}
    BaseLock(SharedMutex& mutex)
        : mutex_(&mutex), owns_(true)
    {
        if (write)
            mutex_->lock();
        else
            mutex_->lock_shared();
    }
    BaseLock(const BaseLock&) = delete;
    BaseLock(BaseLock&& other)
        : mutex_(other.mutex_), owns_(other.owns_)
    {
        other.mutex_ = nullptr;
        other.owns_ = false;
    }

    ~BaseLock(){
        if (owns_){
            if (write)
                mutex_->unlock();
            else
                mutex_->unlock_shared();
        }
    }

    BaseLock& operator=(const BaseLock&) = delete;
    BaseLock& operator=(BaseLock&& other){
        if (owns_){
            if (write)
                mutex_->unlock();
            else
                mutex_->unlock_shared();
        }
        mutex_ = other.mutex_;
        owns_ = other.owns_;
        other.mutex_ = nullptr;
        other.owns_ = false;
    }
};

class WriteLock : BaseLock<true> {
 public:
    using BaseLock::BaseLock;
    void lock(){
        mutex_->lock();
        owns_ = true;
    }
    void unlock(){
        mutex_->unlock();
        owns_ = false;
    }
};

class ReadLock : BaseLock<false> {
 public:
    using BaseLock::BaseLock;
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
