#include "Sync.h"
#include "Utility.h"

#if VST_HOST_SYSTEM == VST_WINDOWS
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
# include <malloc.h>
#else
# include <stdlib.h>
#endif

#include <climits>

// for PaddedSpinLock
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

namespace vst {

/*/////////////////// SyncCondition ////////////////////*/

SyncCondition::SyncCondition(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    InitializeConditionVariable((PCONDITION_VARIABLE)&condition_);
    InitializeSRWLock((PSRWLOCK)&mutex_);
#else // pthreads
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&condition_, 0);
#endif
}

SyncCondition::~SyncCondition(){
#if VST_HOST_SYSTEM != VST_WINDOWS
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&condition_);
#endif
}

void SyncCondition::set(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    AcquireSRWLockExclusive((PSRWLOCK)&mutex_);
    state_ = true;
    ReleaseSRWLockExclusive((PSRWLOCK)&mutex_);

    WakeConditionVariable((PCONDITION_VARIABLE)&condition_);
#else
    pthread_mutex_lock(&mutex_);
    state_ = true;
    pthread_mutex_unlock(&mutex_);

    pthread_cond_signal(&condition_);
#endif
}

void SyncCondition::wait(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    AcquireSRWLockExclusive((PSRWLOCK)&mutex_);
    while (!state_){
        SleepConditionVariableSRW((PCONDITION_VARIABLE)&condition_,
                                  (PSRWLOCK)&mutex_, INFINITE, 0);
    }
    state_ = false;
    ReleaseSRWLockExclusive((PSRWLOCK)&mutex_);
#else
    pthread_mutex_lock(&mutex_);
    while (!state_){
        pthread_cond_wait(&condition_, &mutex_);
    }
    state_ = false;
    pthread_mutex_unlock(&mutex_);
#endif
}

/*/////////////////// Semaphore ////////////////////*/

Semaphore::Semaphore(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    sem_ = CreateSemaphoreA(0, 0, INT_MAX, 0);
#elif VST_HOST_SYSTEM == VST_MACOS
    semaphore_create(mach_task_self(), &sem_, SYNC_POLICY_FIFO, 0);
#else // Linux
    sem_init(&sem_, 0, 0);
#endif
}

Semaphore::~Semaphore(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    CloseHandle(sem_);
#elif VST_HOST_SYSTEM == VST_MACOS
    semaphore_destroy(mach_task_self(), sem_);
#else // Linux
    sem_destroy(&sem_);
#endif
}

void Semaphore::post(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    ReleaseSemaphore(sem_, 1, 0);
#elif VST_HOST_SYSTEM == VST_MACOS
    semaphore_signal(sem_);
#else // Linux
    sem_post(&sem_);
#endif
}

void Semaphore::wait(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    WaitForSingleObject(sem_, INFINITE);
#elif VST_HOST_SYSTEM == VST_MACOS
    semaphore_wait(sem_);
#else // Linux
    while (sem_wait(&sem_) == -1 && errno == EINTR) continue;
#endif
}

/*///////////////////// SpinLock ////////////////////////*/

void SpinLock::yield(){
#if defined(CPU_INTEL)
    _mm_pause();
#elif defined(CPU_ARM)
    __yield();
#else // fallback
    std::this_thread::sleep_for(std::chrono::microseconds(0));
#endif
}

/*///////////////////// PaddedSpinLock ////////////////////////*/

PaddedSpinLock::PaddedSpinLock(){
    static_assert(sizeof(PaddedSpinLock) == CACHELINE_SIZE, "");
    if ((reinterpret_cast<uintptr_t>(this) & (CACHELINE_SIZE-1)) != 0){
        LOG_WARNING("PaddedSpinLock is not properly aligned!");
    }
}

#if __cplusplus < 201703L
void* PaddedSpinLock::operator new(size_t size){
    // Wine doesn't seem to have _aligned_malloc/_aligned_free
#if defined(_WIN32) && !defined(__WINE__)
    void *ptr = _aligned_malloc(size, alignof(PaddedSpinLock));
#else
    void *ptr = nullptr;
    posix_memalign(&ptr, alignof(PaddedSpinLock), size);
    if (!ptr){
        LOG_WARNING("posix_memalign() failed");
        ptr = malloc(size);
    }
#endif
    if (!ptr){
        throw std::bad_alloc();
    }
    return ptr;
}

void PaddedSpinLock::operator delete(void* ptr){
    // see above
#if defined(_WIN32) && !defined(__WINE__)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void *PaddedSpinLock::operator new[](size_t size){
    return PaddedSpinLock::operator new(size);
}

void PaddedSpinLock::operator delete[](void *ptr){
    return PaddedSpinLock::operator delete(ptr);
}
#endif

/*////////////////////// SharedMutex ///////////////////*/

#if VST_HOST_SYSTEM == VST_WINDOWS

Mutex::Mutex() {
    InitializeSRWLock((PSRWLOCK)& lock_);
}
// exclusive
void Mutex::lock() {
    AcquireSRWLockExclusive((PSRWLOCK)&lock_);
}
bool Mutex::try_lock() {
    return TryAcquireSRWLockExclusive((PSRWLOCK)&lock_);
}
void Mutex::unlock() {
    ReleaseSRWLockExclusive((PSRWLOCK)&lock_);
}
// shared
void SharedMutex::lock_shared() {
    AcquireSRWLockShared((PSRWLOCK)&lock_);
}
bool SharedMutex::try_lock_shared() {
    return TryAcquireSRWLockShared((PSRWLOCK)&lock_);
}
void SharedMutex::unlock_shared() {
    ReleaseSRWLockShared((PSRWLOCK)&lock_);
}

#endif

} // vst
