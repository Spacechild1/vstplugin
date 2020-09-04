#include "Sync.h"
#include "Utility.h"

#ifdef _WIN32
# include <windows.h>
# include <malloc.h>
#else
# include <stdlib.h>
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
    condition_.wait(lock, [this](){ return state_; });
    state_ = false;
#endif
}

/*///////////////////// SpinLock ////////////////////////*/

SpinLock::SpinLock(){
    static_assert(sizeof(SpinLock) == CACHELINE_SIZE, "");
    if ((reinterpret_cast<uintptr_t>(this) & (CACHELINE_SIZE-1)) != 0){
        LOG_WARNING("SpinLock is not properly aligned!");
    }
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

bool SpinLock::try_lock(){
    return !locked_.exchange(true, std::memory_order_acquire);
}

void SpinLock::unlock(){
    locked_.store(false, std::memory_order_release);
}

#if __cplusplus < 201703L
void* SpinLock::operator new(size_t size){
#ifdef _WIN32
    void *ptr = _aligned_malloc(size, alignof(SpinLock));
#else
    void *ptr = aligned_alloc(alignof(SpinLock), size);
    if (!ptr){
        ptr = malloc(size);
    }
#endif
    if (!ptr){
        throw std::bad_alloc();
    }
    return ptr;
}

void SpinLock::operator delete(void* ptr){
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
#endif

void *SpinLock::operator new[](size_t size){
    return SpinLock::operator new(size);
}

void SpinLock::operator delete[](void *ptr){
    return SpinLock::operator delete(ptr);
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
