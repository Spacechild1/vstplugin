#include "Sync.h"
#include "Utility.h"

#ifdef _WIN32
# include <windows.h>
# include <malloc.h>
#else
# include <stdlib.h>
#endif

// for SpinLock
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
#ifdef _WIN32
    InitializeConditionVariable((PCONDITION_VARIABLE)&condition_);
    InitializeSRWLock((PSRWLOCK)&mutex_);
#else // pthreads
    pthread_mutex_init(&mutex_, 0);
    pthread_cond_init(&condition_, 0);
#endif
}

SyncCondition::~SyncCondition(){
#ifndef _WIN32
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&condition_);
#endif
}

void SyncCondition::set(){
#if defined(_WIN32)
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
#ifdef _WIN32
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
#if defined(_WIN32)
    sem_ = CreateSemaphoreA(0, 0, LONG_MAX, 0);
#elif defined(__APPLE__)
    semaphore_create(mach_task_self(), &sem_, SYNC_POLICY_FIFO, 0);
#else // pthreads
    sem_init(&sem_, 0, 0);
#endif
}

Semaphore::~Semaphore(){
#if defined(_WIN32)
    CloseHandle(sem_);
#elif defined(__APPLE__)
    semaphore_destroy(mach_task_self(), sem_);
#else // pthreads
    sem_destroy(&sem_);
#endif
}

void Semaphore::post(){
#if defined(_WIN32)
    ReleaseSemaphore(sem_, 1, 0);
#elif defined(__APPLE__)
    semaphore_signal(sem_);
#else
    sem_post(&sem_);
#endif
}

void Semaphore::wait(){
#if defined(_WIN32)
    WaitForSingleObject(sem_, INFINITE);
#elif defined(__APPLE__)
    semaphore_wait(sem_);
#else
    while (sem_wait(&sem_) == -1 && errno == EINTR) continue;
#endif
}

/*///////////////////// SpinLock ////////////////////////*/

SpinLock::SpinLock(){
    static_assert(sizeof(SpinLock) == CACHELINE_SIZE, "");
    if ((reinterpret_cast<uintptr_t>(this) & (CACHELINE_SIZE-1)) != 0){
        LOG_WARNING("SpinLock is not properly aligned!");
    }
}

void SpinLock::yield(){
#if defined(CPU_INTEL)
    _mm_pause();
#elif defined(CPU_ARM)
    __yield();
#else // fallback
    std::this_thread::sleep_for(std::chrono::microseconds(0));
#endif
}

#if __cplusplus < 201703L
void* SpinLock::operator new(size_t size){
#ifdef _WIN32
    void *ptr = _aligned_malloc(size, alignof(SpinLock));
#else
    void *ptr = nullptr;
    posix_memalign(&ptr, alignof(SpinLock), size);
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

void SpinLock::operator delete(void* ptr){
#ifdef _WIN32
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

void *SpinLock::operator new[](size_t size){
    return SpinLock::operator new(size);
}

void SpinLock::operator delete[](void *ptr){
    return SpinLock::operator delete(ptr);
}
#endif

/*////////////////////// SharedMutex ///////////////////*/

#ifdef _WIN32
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
