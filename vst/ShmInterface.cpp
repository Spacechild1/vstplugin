#include "ShmInterface.h"

#include "Log.h"
#include "MiscUtils.h"

#include <cstring>

#if VST_HOST_SYSTEM == VST_WINDOWS
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <unistd.h>
# include <sys/types.h>
# include <fcntl.h>
# include <sys/shm.h>
# include <sys/mman.h>
# if SHM_FUTEX
#  include <sys/syscall.h>
#  include <linux/futex.h>
# else // semaphore
#  include <semaphore.h>
#  if VST_HOST_SYSTEM == VST_MACOS
#   include <sys/stat.h>
#  endif
# endif
#endif

#ifndef DEBUG_SHM
#define DEBUG_SHM 0
#endif

#if DEBUG_SHM
#define LOG_SHM(x) LOG_DEBUG(x)
#else
#define LOG_SHM(x)
#endif

namespace vst {

#if SHM_FUTEX
int futex(std::atomic<uint32_t>* uaddr, int futex_op, uint32_t val,
          const struct timespec *timeout, uint32_t *uaddr2, uint32_t val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

void futex_wait(std::atomic<uint32_t>* futexp)
{
    for (;;) {
        // is futex available?
        uint32_t expected = 1;
        if (futexp->compare_exchange_strong(expected, 0)){
            // success
            break;
        }
        // not available - wait
        auto ret = futex(futexp, FUTEX_WAIT, 0, nullptr, nullptr, 0);
        if (ret < 0 && errno != EAGAIN){
            throw Error(Error::SystemError,
                        "futex_wait() failed: " + errorMessage(errno));
        }
    }
}

void futex_post(std::atomic<uint32_t>* futexp)
{
    uint32_t expected = 0;
    if (futexp->compare_exchange_strong(expected, 1)) {
       // wake one waiter
       if (futex(futexp, FUTEX_WAKE, 1, nullptr, nullptr, 0) < 0) {
           throw Error(Error::SystemError,
                       "futex_post() failed: " + errorMessage(errno));
       }
    }
}
#endif

constexpr size_t align_to(size_t s, size_t alignment){
    auto mask = alignment - 1;
    return (s + mask) & ~mask;
}

/*/////////////// ShmChannel ////////////*/


ShmChannel::ShmChannel(Type type, int32_t size,
                       const std::string& name)
    : owner_(true), type_(type), bufferSize_(size), name_(name)
{
#if SHM_FUTEX || SHM_EVENT
    static_assert(sizeof(Header) == 64, "bad size for Header");
#else
    static_assert(sizeof(Header) == 128, "bad size for Header");
#endif
    auto total = sizeof(Header) + sizeof(Data) + size;
    totalSize_ = align_to(total, alignment);
}

void ShmChannel::HandleDeleter::operator ()(void *handle){
#if SHM_EVENT
    // LOG_SHM("close event " << handle);
    CloseHandle((HANDLE)handle);
#elif SHM_SEMAPHORE
    sem_close((sem_t *)handle);
#endif
    // nothing to do for SHM_FUTEX
}

ShmChannel::~ShmChannel(){
    // LOG_SHM("~ShmChannel");
}

size_t ShmChannel::peekMessage() const {
    if (data_->size.load(std::memory_order_relaxed) > 0){
        auto msg = (Message *)&data_->data[rdhead_];
        return msg->size;
    } else {
        return 0;
    }
}

bool ShmChannel::readMessage(char * buffer, size_t& size) {
    if (data_->size.load(std::memory_order_relaxed) > 0){
        auto msg = (Message *)&data_->data[rdhead_];
        if (msg->size > size){
            size = msg->size; // return required minimum size
            return false;
        }

        auto begin = msg->data;
        auto end = begin + msg->size;
        auto limit = data_->data + data_->capacity;
        auto msgsize = msg->size + sizeof(msg->size);

        // >= ensures rdhead wrap around!
        if (end >= limit){
            auto n1 = limit - begin;
            auto n2 = end - limit;
            memcpy(buffer, begin, n1);
            memcpy(buffer + n1, data_->data, n2);
            rdhead_ = n2;
        } else {
            memcpy(buffer, begin, msg->size);
            rdhead_ += msgsize;
        }

        data_->size -= msgsize; // atomic decrement!

        size = msg->size; // return actual size
        return true;
    } else {
        size = 0;
        return false;
    }
}

bool ShmChannel::writeMessage(const char *data, size_t size) {
    auto capacity = data_->capacity;
    // get actual message size (+ size field + alignment)
    auto msgsize = align_to(size + sizeof(Message::size), Message::alignment);
    if ((capacity - data_->size.load(std::memory_order_relaxed)) >= msgsize){
        auto msg = (Message *)&data_->data[wrhead_];

        msg->size = msgsize - sizeof(msg->size); // minus size field!

        auto begin = msg->data;
        auto end = begin + size; // use original size!
        auto limit = data_->data + capacity;

        if (end > limit){
            auto n1 = limit - begin;
            auto n2 = end - limit;
            memcpy(begin, data, n1);
            memcpy(data_->data, data + n1, n2);
        } else {
            memcpy(begin, data, size); // use original size!
        }

        // we have to handle wrhead seperately because msg->size != size
        wrhead_ += msgsize;
        if (wrhead_ >= capacity){
            wrhead_ -= capacity;
        }

        data_->size.fetch_add(msgsize, std::memory_order_release);

        return true;
    } else {
        return false;
    }
}

bool ShmChannel::addMessage(const char * data, size_t size) {
    auto capacity = data_->capacity;
    // get actual message size (+ size field + alignment)
    auto msgsize = align_to(size + sizeof(Message::size), Message::alignment);
    if ((capacity - data_->size.load(std::memory_order_relaxed)) >= msgsize){
        auto msg = (Message *)&data_->data[wrhead_];

        msg->size = msgsize - sizeof(msg->size); // minus size field!
        memcpy(msg->data, data, size); // use original size!

        wrhead_ += msgsize;
        data_->size.fetch_add(msgsize, std::memory_order_release);

        return true;
    } else {
        return false;
    }
}

bool ShmChannel::getMessage(const char *& buf, size_t& size) {
    if (data_->size.load(std::memory_order_relaxed) > 0){
        auto msg = (Message *)&data_->data[rdhead_];

        buf = msg->data;
        size = msg->size;

        auto msgsize = msg->size + sizeof(msg->size);
        rdhead_ += msgsize;
        data_->size -= msgsize; // atomic decrement!

        return true;
    } else {
        return false;
    }
}

void ShmChannel::clear(){
    data_->size = 0;
    reset();
}

void ShmChannel::reset(){
    rdhead_ = 0;
    wrhead_ = 0;
}

void ShmChannel::post(){
    postEvent(eventA_.get());
}

void ShmChannel::wait(){
    waitEvent(eventA_.get());
}

void ShmChannel::postReply(){
    postEvent(eventB_.get());
}

void ShmChannel::waitReply(){
    waitEvent(eventB_.get());
}

void ShmChannel::init(ShmInterface& shm, char *data, int num){
    LOG_SHM("init channel " << num);
    header_ = reinterpret_cast<Header *>(data);
    if (owner_){
        // placement new
        new (header_) Header(type_, name_.c_str(), totalSize_);
    #if SHM_SEMAPHORE
        // POSIX expects leading slash
        snprintf(header_->data1, sizeof(header_->data1),
                 "/vst_shm_%p_%da", &shm, num);
        if (type_ == Request){
            snprintf(header_->data2, sizeof(header_->data2),
                     "/vst_shm_%p_%db", &shm, num);
        } else {
            header_->data2[0] = '\0';
        }
    #endif
    } else {
        if (header_->offset != sizeof(Header)){
            throw Error(Error::SystemError, "shared memory interface not compatible (wrong header size)!");
        }
        totalSize_ = header_->size;
        type_ = (Type)header_->type;
        name_ = header_->name;
    }

    initEvent(shm, eventA_, &header_->data1);
    if (type_ == Request){
        initEvent(shm, eventB_, &header_->data2);
    }

    if (owner_){
        // placement new
        data_ = new (data + header_->offset) Data();
        data_->capacity = bufferSize_;
    } else {
        data_ = reinterpret_cast<Data *>(data + header_->offset);
    }

    LOG_SHM("init ShmChannel " << num << " (" << name_
              << "): buffer size = " << data_->capacity
              << ", total size = " << totalSize_
              << ", start address = " << (void *)data);
}

void ShmChannel::initEvent(ShmInterface& shm, Handle& event, void *data){
    // LOG_SHM("ShmChannel: init event " << which);
#if SHM_EVENT
    if (owner_){
        // create new Event
        event.reset(CreateEventA(0, 0, 0, 0));
        if (!event){
            throw Error(Error::SystemError, "CreateEvent() failed: "
                        + errorMessage(GetLastError()));
        }
        // HANDLE can be safely truncated to 32-bit integer
        uint32_t handle = (uint32_t)reinterpret_cast<uintptr_t>(event.get());
        memcpy(data, &handle, sizeof(handle));
    } else {
        // get Event handle from shared memory segment and duplicate it
        // to get a handle that is valid in this process.
        uint32_t intHandle;
        memcpy(&intHandle, data, sizeof(uint32_t));
        auto sourceHandle = reinterpret_cast<HANDLE>(intHandle);
        auto sourceProcess = (HANDLE)shm.getParentProcessHandle();
        auto targetProcess = GetCurrentProcess();
        HANDLE targetHandle;
        auto result = DuplicateHandle(sourceProcess, sourceHandle,
                                      targetProcess, &targetHandle,
                                      0, FALSE, DUPLICATE_SAME_ACCESS);
        if (!result){
            throw Error(Error::SystemError, "DuplicateHandle() failed: "
                        + errorMessage(GetLastError()));
        }
        event.reset(targetHandle);
    }
    LOG_SHM("create event " << event.get());
#elif SHM_SEMAPHORE
    if (owner_){
        // create semaphore and return an error if it already exists
        event.reset(sem_open((const char *)data, O_CREAT | O_EXCL, 0755, 0));
        LOG_SHM("ShmChannel: created semaphore " << (const char *)data);
    } else {
        // open an existing semaphore
        event.reset(sem_open((const char *)data, 0, 0, 0, 0));
        LOG_SHM("ShmChannel: created semaphore " << (const char *)data);
    }
    if (event.get() == SEM_FAILED){
        throw Error(Error::SystemError, "sem_open() failed: "
                    + errorMessage(errno));
    }
#elif SHM_FUTEX
    event.reset(data);
#endif
}

void ShmChannel::postEvent(void *event){
#if SHM_EVENT
    if (!SetEvent((HANDLE)event)){
        throw Error(Error::SystemError, "SetEvent() failed: "
                    + errorMessage(GetLastError()));
    }
#elif SHM_SEMAPHORE
    if (sem_post((sem_t *)event) != 0){
        throw Error(Error::SystemError, "sem_post() failed: "
                    + errorMessage(errno));
    }
#elif SHM_FUTEX
    futex_post(static_cast<std::atomic<uint32_t>*>(event));
#endif
}

void ShmChannel::waitEvent(void *event){
#if SHM_EVENT
    auto result = WaitForSingleObject(event, INFINITE);
    if (result != WAIT_OBJECT_0){
        if (result == WAIT_ABANDONED){
            LOG_ERROR("WaitForSingleObject() failed! Event abandoned");
        } else {
            throw Error(Error::SystemError, "WaitForSingleObject() failed: "
                        + errorMessage(GetLastError()));
        }
    }
#elif SHM_SEMAPHORE
    if (sem_wait((sem_t *)event) != 0){
        throw Error(Error::SystemError, "sem_wait() failed: "
                    + errorMessage(errno));
    }
#elif SHM_FUTEX
    futex_wait(static_cast<std::atomic<uint32_t>*>(event));
#endif
}

/*//////////////// ShmInterface //////////////////*/

ShmInterface::Header::Header(uint32_t _size, uint32_t _numChannels) {
    size = _size;
    versionMajor = VERSION_MAJOR;
    versionMinor = VERSION_MINOR;
    versionPatch = VERSION_PATCH;
#if SHM_EVENT
    processID = GetCurrentProcessId();
#endif
    numChannels = _numChannels;
    memset(channelOffset, 0, sizeof(channelOffset));
}

ShmInterface::ShmInterface(){}

ShmInterface::~ShmInterface(){
    closeShm();
    LOG_SHM("closed ShmInterface");

#if SHM_EVENT
    CloseHandle(hParentProcess_);
#endif
}

void ShmInterface::connect(const std::string &path){
    if (data_){
        throw Error(Error::SystemError, "ShmInterface: already connected()!");
    }

    openShm(path, false);
    LOG_SHM("ShmInterface: connected to " << path);
    auto header = reinterpret_cast<Header *>(data_);
    LOG_SHM("total size: " << header->size);

#if SHM_EVENT
    // needed for channel events
    hParentProcess_ = OpenProcess(PROCESS_DUP_HANDLE, FALSE, header->processID);
    if (!hParentProcess_){
        throw Error(Error::SystemError, "OpenProcess() failed: "
                    + errorMessage(GetLastError()));
    }
#endif

    // channels_.reserve(header->numChannels);
    for (size_t i = 0; i < header->numChannels; ++i){
        channels_.emplace_back();
        channels_[i].init(*this, data_ + header->channelOffset[i], i);
    }
}

void ShmInterface::disconnect(){
    if (data_){
        if (!owner_){
            closeShm();
        } else {
            LOG_WARNING("ShmInterface: owner must not call disconnect()!");
        }

    } else {
        LOG_WARNING("ShmInterface::disconnect: not connected");
    }
}

void ShmInterface::addChannel(ShmChannel::Type type,
                              size_t size, const std::string &name)
{
    if (data_){
        throw Error(Error::SystemError,
                    "ShmInterface: must not call addChannel() after create()!");
    }

    if (channels_.size() == maxNumChannels){
        throw Error(Error::SystemError,
                    "ShmInterface: max. number of channels reached!");
    }
    channels_.emplace_back(type, size, name);
}

void ShmInterface::create(){
    if (data_){
        throw Error(Error::SystemError, "ShmInterface: already created()!");
    }

    char path[64];
    // POSIX expects leading slash
    snprintf(path, sizeof(path), "/vst_shm_%p", this);

    openShm(path, true);
    LOG_SHM("ShmInterface: created " << path);
    LOG_SHM("total size: " << size_);

    // placement new
    auto header = new (data_) Header(size_, channels_.size());
    char *ptr = data_ + sizeof(Header);

    for (size_t i = 0; i < channels_.size(); ++i){
        channels_[i].init(*this, ptr, i);
        header->channelOffset[i] = ptr - data_;
        ptr += channels_[i].size();
    }
}

void ShmInterface::close(){
    if (data_){
        if (owner_){
            closeShm();
        } else {
            LOG_WARNING("ShmInterface: only owner may call close()!");
        }
    } else {
        LOG_WARNING("ShmInterface::close: not connected");
    }
}

void ShmInterface::openShm(const std::string &path, bool create){
    size_t totalSize = sizeof(Header);

    if (create) {
        for (auto& chn : channels_) {
            totalSize += chn.size();
        }
    }

#if VST_HOST_SYSTEM == VST_WINDOWS
    HANDLE hMapFile;
    if (create){
        hMapFile = CreateFileMappingA(
            INVALID_HANDLE_VALUE,    // use paging file
            NULL,                    // default security
            PAGE_READWRITE,          // read/write access
            0,                       // maximum object size (high-order DWORD)
            totalSize,               // maximum object size (low-order DWORD)
            path.c_str());           // name of mapping object

        if (!hMapFile)
        {
            throw Error(Error::SystemError, "CreateFileMapping() failed: "
                        + errorMessage(GetLastError()));
        }
    } else {
        hMapFile = OpenFileMappingA(
            FILE_MAP_ALL_ACCESS,   // read/write access
            FALSE,                 // do not inherit the name
            path.c_str());         // name of mapping object

        if (!hMapFile){
            throw Error(Error::SystemError, "OpenFileMapping() failed: "
                        + errorMessage(GetLastError()));
        }
    }
    void *data = MapViewOfFile(hMapFile, // handle to map object
                               FILE_MAP_ALL_ACCESS, // read/write permission
                               0, 0, totalSize);    // size
    if (data && !create){
        // get actual total size
        totalSize = static_cast<Header *>(data)->size;
        UnmapViewOfFile(data);
        // map again with correct size
        data = MapViewOfFile(hMapFile,
                             FILE_MAP_ALL_ACCESS,
                             0, 0, totalSize);
    }

    if (!data){
        CloseHandle(hMapFile);
        throw Error(Error::SystemError, "MapViewOfFile() failed: "
                    + errorMessage(GetLastError()));
    }

    // try to lock the file to physical memory
    // first we have to increase the minimum working set size
    SIZE_T minSize, maxSize;
    if (GetProcessWorkingSetSize(GetCurrentProcess(), &minSize, &maxSize)){
        LOG_SHM("working set size: min = " << minSize << ", max = " << maxSize);
        LOG_SHM("request size: " << totalSize);
        if (totalSize > minSize){
            minSize += totalSize;
        }
        if (totalSize > maxSize){
            maxSize += totalSize;
        }
       if (!SetProcessWorkingSetSize(GetCurrentProcess(), minSize, maxSize)){
            LOG_WARNING("ShmInterface: SetProcessWorkingSetSize() failed: "
                        << GetLastError());
        }
    } else {
        LOG_WARNING("ShmInterface: GetProcessWorkingSetSize() failed: "
                    << GetLastError());
    }
    // now we can attempt to lock the memory
    if (!VirtualLock(data, totalSize)){
        LOG_WARNING("ShmInterface: VirtualLock() failed: "
                    << GetLastError());
    }
#else
    int fd;
    if (create){
        fd = shm_open(path.c_str(), O_CREAT | O_RDWR | O_EXCL, 0666);
    } else {
        fd = shm_open(path.c_str(), O_RDWR, 0666);
    }
    if (fd < 0){
        throw Error(Error::SystemError, "shm_open() failed: "
                    + errorMessage(errno));
    }
    if (create){
        // configure size of shared memory object
        if (ftruncate(fd, totalSize) != 0){
            ::close(fd);
            shm_unlink(path.c_str());
            throw Error(Error::SystemError, "ftruncate() failed: "
                        + errorMessage(errno));
        }
    }
    // memory map the shared memory object
    int err = 0;
    void *data = mmap(0, totalSize, PROT_WRITE, MAP_SHARED, fd, 0);
    if (!data) {
        err = errno; // cache errno
    } else if (!create) {
        // get actual total size
        auto oldSize = totalSize;
        totalSize = static_cast<Header *>(data)->size;
        munmap(data, oldSize);
        // map again with correct size
        data = mmap(0, totalSize, PROT_WRITE, MAP_SHARED, fd, 0);
        if (!data) {
            err = errno; // cache errno
        }
    }
    // we can close the fd after calling mmap()!
    ::close(fd);

    if (!data){
        if (create){
            shm_unlink(path.c_str());
        }
        throw Error(Error::SystemError, "mmap() failed: "
                    + errorMessage(err));
    }
#if 1
    // try to lock the file to physical memory
    if (mlock(data, totalSize) != 0){
        LOG_WARNING("ShmInterface: mlock() failed: "
                    << strerror(errno));
    }
#endif

#endif // Unix

    // success!
    path_ = path;
    owner_ = create;
#if VST_HOST_SYSTEM == VST_WINDOWS
    hMapFile_ = hMapFile;
#endif
    data_ = (char *)data;
    size_ = totalSize;

    if (create){
        // zero the memory region. this also ensures
        // that everything will be paged in.
        memset(data, 0, totalSize);
    }
}

void ShmInterface::closeShm(){
#if VST_HOST_SYSTEM == VST_WINDOWS
    if (data_){
        UnmapViewOfFile(data_);
    }
    if (hMapFile_){
        CloseHandle(hMapFile_);
        hMapFile_ = nullptr;
    }
#else
    if (data_){
        munmap(data_, size_);
        if (owner_){
            shm_unlink(path_.c_str());
        }
    }
#endif
    path_.clear();
    data_ = nullptr;
    size_ = 0;
    channels_.clear();
}

void ShmInterface::getVersion(int& major, int& minor, int& patch) const {
    auto header = reinterpret_cast<const Header *>(data_);
    major = header->versionMajor;
    minor = header->versionMinor;
    patch = header->versionPatch;
}

} // vst
