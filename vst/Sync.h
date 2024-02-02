#pragma once

// only for VST_HOST_SYSTEM
#include "Interface.h"
#include "Log.h"
#include "MiscUtils.h"

#include <stddef.h>

// for SpinLock
// Intel
#if defined(__i386__) || defined(_M_IX86) || defined(__x86_64__) || defined(_M_X64)
#define CPU_INTEL
#include <immintrin.h>
#endif
// ARM
#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__)
#define CPU_ARM
#endif

// Wine executables segfaults during static initialization
// of mutex/event/semaphore objects which use the Win32 API,
// so we rather use the host system's synchronization primitives.
#if VST_HOST_SYSTEM != VST_WINDOWS
# include <pthread.h>
# if VST_HOST_SYSTEM == VST_MACOS
   // macOS doesn't support unnamed pthread semaphores,
   // so we use Mach semaphores instead
#  include <mach/mach.h>
# else
   // unnamed pthread semaphore
#  include <semaphore.h>
# endif
#endif

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>

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
#if VST_HOST_SYSTEM == VST_WINDOWS
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
    void post(int count);
    void wait();
 private:
#if VST_HOST_SYSTEM == VST_WINDOWS
    void *sem_;
#elif VST_HOST_SYSTEM == VST_MACOS
    semaphore_t sem_;
#else // Linux
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
    void post(int count){
        auto old = count_.fetch_add(count, std::memory_order_release);
        if (old < 0){
            auto release = -old < count ? -old : count;
            sem_.post(release);
        }
    }
    void wait(){
        auto old = count_.fetch_sub(1, std::memory_order_acquire);
        if (old <= 0){
            sem_.wait();
        }
    }
    bool try_wait(){
        auto value = count_.load(std::memory_order_relaxed);
        // NOTE: we could also use a single compare_exchange_strong(),
        // but I don't think that try_wait() should fail just because
        // another thread decremented the count concurrently.
        while (value > 0) {
            if (count_.compare_exchange_weak(value, value - 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            } // retry
        }
        return false;
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
            // increment 'oldcount', because another thread
            // might decrement the counter concurrently!
            auto newcount = oldcount >= 0 ? 1 : oldcount + 1;
            if (count_.compare_exchange_weak(oldcount, newcount,
                    std::memory_order_release, std::memory_order_relaxed)) {
                break;
            }
        }
        if (oldcount < 0){
            sem_.post(); // release one waiting thread
        }
    }
    void wait(){
        auto old = count_.fetch_sub(1, std::memory_order_acquire);
        if (old <= 0){
            sem_.wait();
        }
    }
    bool try_wait(){
        auto value = count_.load(std::memory_order_relaxed);
        // NOTE: we could also use a single compare_exchange_strong(),
        // but I don't think that try_wait() should fail just because
        // another thread decremented the count concurrently.
        while (value > 0) {
            if (count_.compare_exchange_weak(value, value - 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            } // retry
        }
        return false;
    }
 private:
    Semaphore sem_;
    std::atomic<int32_t> count_{0};
};

/*///////////////////// SpinLock /////////////////////*/

inline void pauseCpu() {
#if defined(CPU_INTEL)
    _mm_pause();
#elif defined(CPU_ARM)
    __asm__ __volatile__("yield");
#else // fallback
    std::this_thread::yield();
#endif
}

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
                pauseCpu();
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
    // data
    std::atomic<int32_t> locked_{false};
};


const size_t CACHELINE_SIZE = 64;

class alignas(CACHELINE_SIZE) PaddedSpinLock
    : public SpinLock, public AlignedClass<PaddedSpinLock> {
 public:
    PaddedSpinLock() {
        static_assert(sizeof(PaddedSpinLock) == CACHELINE_SIZE, "");
        if ((reinterpret_cast<uintptr_t>(this) & (CACHELINE_SIZE-1)) != 0){
            LOG_WARNING("PaddedSpinLock is not properly aligned!");
        }
    }
 private:
    // pad and align to prevent false sharing
    char pad_[CACHELINE_SIZE - sizeof(locked_)];
};

/*//////////////////////// SharedMutex //////////////////////////*/

// The std::mutex implementation on Windows is bad on both MSVC and MinGW:
// the MSVC version apparantely has some additional overhead; winpthreads (MinGW) doesn't even use the obvious
// platform primitive (SRWLOCK), they rather roll their own mutex based on atomics and Events, which is bad for our use case.
// Even on Linux and macOS, there's some overhead for things we don't need, so we use pthreads directly.
#if VST_HOST_SYSTEM == VST_WINDOWS
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

/*//////////////////////// SharedMutex ///////////////////////////*/

#if VST_HOST_SYSTEM == VST_WINDOWS
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

} //  vst
