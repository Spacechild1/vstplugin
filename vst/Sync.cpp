#include "Sync.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace vst {

/*/////////////////// Event ////////////////////*/

Event::Event(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    event_ = CreateEvent(nullptr, 0, 0, nullptr);
#elif defined(__APPLE__)
    sem_ = dispatch_semaphore_create(0);
#else // pthreads
    sem_init(&sem_, 0, 0);
#endif
#endif // USE_PLATFORM_EVENT
}

Event::~Event(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    CloseHandle(event_);
#elif defined(__APPLE__)
    dispatch_release(sem_);
#else // pthreads
    sem_destroy(&sem_);
#endif
#endif // USE_PLATFORM_EVENT
}

void Event::signal(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    SetEvent(event_);
#elif defined(__APPLE__)
    dispatch_semaphore_signal(sem_);
#else
    sem_post(&sem_);
#endif
#else // USE_PLATFORM_EVENT
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = true;
    condition_.notify_one();
#endif
}

void Event::wait(){
#if USE_PLATFORM_EVENT
#if defined(_WIN32)
    WaitForSingleObject(event_, INFINITE);
#elif defined(__APPLE__)
    dispatch_semaphore_wait(sem_, DISPATCH_TIME_FOREVER);
#else
    sem_wait(&sem_);
#endif
#else // USE_PLATFORM_EVENT
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [](){ return state_; });
    state_ = false;
#endif
}

/*///////////////////// SpinLock ////////////////////////*/

SpinLock::SpinLock(){
    static_assert(sizeof(SpinLock) == CACHELINE_SIZE, "");
}

void SpinLock::lock(){
    // only try to modify the shared state if the lock seems to be available.
    // this should prevent unnecessary cache invalidation.
    do {
        while (locked_.load(std::memory_order_relaxed)){
        #if defined(CPU_INTEL)
            _mm_pause();
        #elif defined(CPU_ARM)
            __yield();
        #else // fallback
            std::this_thread::sleep_for(std::chrono::microseconds(0));
        #endif
        }
    } while (locked_.exchange(true, std::memory_order_acquire));
}

void SpinLock::unlock(){
    locked_.store(false, std::memory_order_release);
}

/*////////////////////// SharedMutex ///////////////////*/

#ifdef _WIN32
SharedMutex::SharedMutex() {
    InitializeSRWLock((PSRWLOCK)& rwlock_);
}
// exclusive
void SharedMutex::lock() {
    AcquireSRWLockExclusive((PSRWLOCK)&rwlock_);
}
bool SharedMutex::try_lock() {
    return TryAcquireSRWLockExclusive((PSRWLOCK)&rwlock_);
}
void SharedMutex::unlock() {
    ReleaseSRWLockExclusive((PSRWLOCK)&rwlock_);
}
// shared
void SharedMutex::lock_shared() {
    AcquireSRWLockShared((PSRWLOCK)&rwlock_);
}
bool SharedMutex::try_lock_shared() {
    return TryAcquireSRWLockShared((PSRWLOCK)&rwlock_);
}
void SharedMutex::unlock_shared() {
    ReleaseSRWLockShared((PSRWLOCK)&rwlock_);
}

#endif

} // vst
