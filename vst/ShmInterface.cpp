#include "ShmInterface.h"
#include "Interface.h"
#include "Utility.h"

#include <cstring>

#ifdef _WIN32
# include <Windows.h>
#else
# include <unistd.h>
# include <sys/types.h>
# include <fcntl.h>
# include <sys/shm.h>
# include <sys/mman.h>
# include <semaphore.h>
# ifdef __APPLE_
#  include <sys/stat.h>
# endif
#endif

#if 0
#define SHM_DEBUG(x) LOG_DEBUG(x)
#else
#define SHM_DEBUG(x)
#endif

namespace vst {

constexpr size_t align_to(size_t s, size_t alignment){
    auto mask = alignment - 1;
    return (s + mask) & ~mask;
}

/*/////////////// ShmChannel ////////////*/


ShmChannel::ShmChannel(Type type, int32_t size,
                                         const std::string& name)
    : owner_(true), type_(type), bufferSize_(size), name_(name)
{
    auto total = sizeof(Header) + sizeof(Data) + size;
    totalSize_ = align_to(total, alignment);
}

void ShmChannel::HandleDeleter::operator ()(void *handle){
#if defined(_WIN32)
    // SHM_DEBUG("close event " << handle);
    CloseHandle((HANDLE)handle);
#elif defined(__APPLE__)
    sem_close((sem_t *)handle);
#endif
}

ShmChannel::~ShmChannel(){
#if !defined(_WIN32) && !defined(__APPLE__)
    // only destroy the semaphore once!
    if (owner_){
        if (eventA_){
            sem_destroy((sem_t *)eventA_.get());
        }
        if (eventB_){
            sem_destroy((sem_t *)eventB_.get());
        }
    }
#endif
    // SHM_DEBUG("~ShmChannel");
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

        data_->size += msgsize; // atomic increment!

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
        data_->size += msgsize; // atomic increment!

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

void ShmChannel::init(char *data, ShmInterface& shm, int num){
    SHM_DEBUG("init channel " << num);
    header_ = reinterpret_cast<Header *>(data);
    if (owner_){
        header_->size = totalSize_;
        header_->offset = sizeof(Header);
        header_->type = type_;
        snprintf(header_->name, sizeof(header_->name), "%s", name_.c_str());
        // POSIX expects leading slash
        snprintf(header_->event1, sizeof(header_->event1),
                 "/vst_shm_%p_%da", &shm, num);
        if (type_ == Request){
            snprintf(header_->event2, sizeof(header_->event2),
                     "/vst_shm_%p_%db", &shm, num);
        } else {
            header_->event2[0] = '\0';
        }
    } else {
        totalSize_ = header_->size;
        type_ = (Type)header_->type;
        name_ = header_->name;
    }
    SHM_DEBUG("initEvent");
    initEvent(eventA_, header_->event1);
    if (type_ == Request){
        initEvent(eventB_, header_->event2);
    }

    if (owner_){
        // placement new
        data_ = new (data + header_->offset) Data();
        data_->capacity = bufferSize_;
    } else {
        data_ = reinterpret_cast<Data *>(data + header_->offset);
    }

    SHM_DEBUG("init ShmChannel " << num << " (" << name_
              << "): buffer size = " << data_->capacity
              << ", total size = " << totalSize_
              << ", start address = " << (void *)data);
}

void ShmChannel::initEvent(Handle& event, const char *data){
    // SHM_DEBUG("ShmChannel: init event " << which);
#if defined(_WIN32)
    // named Event
    if (owner_){
        event.reset(CreateEventA(0, 0, 0, data));
        if (event){
            if (GetLastError() != ERROR_ALREADY_EXISTS){
                SHM_DEBUG("ShmChannel: created Event " << data);
            } else {
                throw Error(Error::SystemError,
                            "CreateEvent() failed - already exists!");
            }
        } else {
            throw Error(Error::SystemError, "CreateEvent() failed with "
                        + std::to_string(GetLastError()));
        }
    } else {
        event.reset(OpenEventA(EVENT_ALL_ACCESS, 0, data));
        if (event){
            SHM_DEBUG("ShmChannel: opened Event " << data);
        } else {
            throw Error(Error::SystemError, "OpenEvent() failed with "
                        + std::to_string(GetLastError()));
        }
    }
    SHM_DEBUG("create event " << event.get());
#elif defined(__APPLE__)
    // named semaphore
    if (owner_){
        // create semaphore and return an error if it already exists
        event.reset(sem_open(data, O_CREAT | O_EXCL, 0755, 0));
    } else {
        // open an existing semaphore
        event.reset(sem_open(data, 0, 0, 0, 0));
    }
    if (event.get() == SEM_FAILED){
        throw Error(Error::SystemError, "sem_open() failed with "
                    + std::string(strerror(errno)));
    }
#else
    // unnamed semaphore in shared memory segment
    event.reset((void *)data);
    if (owner_){
        // only init the semaphore once!
        if (sem_init((sem_t *)data, 1, 0) != 0){
            throw Error(Error::SystemError, "sem_init() failed: "
                        + std::string(strerror(errno)));
        }
    }
#endif
}

void ShmChannel::postEvent(void *event){
#ifdef _WIN32
    if (!SetEvent((HANDLE)event)){
        throw Error(Error::SystemError, "SetEvent() failed with "
                    + std::to_string(GetLastError()));
    }
#else
    if (sem_post((sem_t *)event) != 0){
        throw Error(Error::SystemError, "sem_post() failed: "
                    + std::string(strerror(errno)));
    }
#endif
}

void ShmChannel::waitEvent(void *event){
#ifdef _WIN32
    auto result = WaitForSingleObject(event, INFINITE);
    if (result != WAIT_OBJECT_0){
        if (result == WAIT_ABANDONED){
            LOG_ERROR("WaitForSingleObject() failed! Event abandoned");
        } else {
            throw Error(Error::SystemError, "WaitForSingleObject() failed with "
                        + std::to_string(GetLastError()));
        }
    }
#else
    if (sem_wait((sem_t *)event) != 0){
        throw Error(Error::SystemError, "sem_wait() failed: "
                    + std::string(strerror(errno)));
    }
#endif
}

/*//////////////// ShmInterface //////////////////*/

ShmInterface::ShmInterface(){}

ShmInterface::~ShmInterface(){
    closeShm();
    SHM_DEBUG("closed ShmInterface");
}

void ShmInterface::connect(const std::string &path){
    if (data_){
        throw Error(Error::SystemError, "ShmInterface: already connected()!");
    }

    openShm(path, false);
    SHM_DEBUG("ShmInterface: connected to " << path);
    auto header = reinterpret_cast<Header *>(data_);
    SHM_DEBUG("total size: " << header->size);

    // channels_.reserve(header->numChannels);
    for (size_t i = 0; i < header->numChannels; ++i){
        channels_.emplace_back();
        channels_[i].init(data_ + header->channelOffset[i], *this, i);
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

    auto header = reinterpret_cast<Header *>(data_);
    memset(header, 0, sizeof(Header));
    header->size = size_;
    header->versionMajor = VERSION_MAJOR;
    header->versionMinor = VERSION_MINOR;
    header->versionBugfix = VERSION_BUGFIX;
    header->numChannels = channels_.size();

    char *ptr = data_ + sizeof(Header);

    for (size_t i = 0; i < channels_.size(); ++i){
        channels_[i].init(ptr, *this, i);
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

#ifdef _WIN32
    HANDLE hMapFile;
    if (create){
        hMapFile = CreateFileMapping(
            INVALID_HANDLE_VALUE,    // use paging file
            NULL,                    // default security
            PAGE_READWRITE,          // read/write access
            0,                       // maximum object size (high-order DWORD)
            totalSize,               // maximum object size (low-order DWORD)
            path.c_str());           // name of mapping object

        if (!hMapFile)
        {
            throw Error(Error::SystemError, "CreateFileMapping() failed with "
                        + std::to_string(GetLastError()));
        }
    } else {
        hMapFile = OpenFileMappingA(
            FILE_MAP_ALL_ACCESS,   // read/write access
            FALSE,                 // do not inherit the name
            path.c_str());         // name of mapping object

        if (!hMapFile){
            throw Error(Error::SystemError, "OpenFileMapping() failed with "
                        + std::to_string(GetLastError()));
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
        throw Error(Error::SystemError, "MapViewOfFile() failed with "
                    + std::to_string(GetLastError()));
    }

    // try to lock the file to physical memory
    // first we have to increase the minimum working set size
    SIZE_T minSize, maxSize;
    if (GetProcessWorkingSetSize(GetCurrentProcess(), &minSize, &maxSize)){
        SHM_DEBUG("working set size: min = " << minSize << ", max = " << maxSize);
        SHM_DEBUG("request size: " << totalSize);
        if (totalSize > minSize){
            minSize += totalSize;
        }
        if (totalSize > maxSize){
            maxSize += totalSize;
        }
       if (!SetProcessWorkingSetSize(GetCurrentProcess(), minSize, maxSize)){
            LOG_WARNING("ShmInterface: SetProcessWorkingSetSize() failed with "
                        << GetLastError());
        }
    } else {
        LOG_WARNING("ShmInterface: GetProcessWorkingSetSize() failed with "
                    << GetLastError());
    }
    // now we can attempt to lock the memory
    if (!VirtualLock(data, totalSize)){
        LOG_WARNING("ShmInterface: VirtualLock() failed with "
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
        throw Error(Error::SystemError, "shm_open() failed with "
                    + std::string(strerror(errno)));
    }
    if (create){
        // configure size of shared memory object
        if (ftruncate(fd, totalSize) != 0){
            ::close(fd);
            shm_unlink(path.c_str());
            throw Error(Error::SystemError, "ftruncate() failed with "
                        + std::string(strerror(errno)));
        }
    }
    // memory map the shared memory object
    void *data = mmap(0, totalSize, PROT_WRITE, MAP_SHARED, fd, 0);
    if (data && !create){
        // get actual total size
        auto oldSize = totalSize;
        totalSize = static_cast<Header *>(data)->size;
        munmap(data, oldSize);
        // map again with correct size
        data = mmap(0, totalSize, PROT_WRITE, MAP_SHARED, fd, 0);
    }
    // we can close the fd after calling mmap()!
    ::close(fd);

    if (!data){
        if (create){
            shm_unlink(path.c_str());
        }
        throw Error(Error::SystemError, "mmap() failed with "
                    + std::string(strerror(errno)));
    }

    // try to lock the file to physical memory
    if (mlock(data, totalSize) != 0){
        LOG_WARNING("ShmInterface: mlock() failed with "
                    << strerror(errno));
    }
#endif

    // success!
    path_ = path;
    owner_ = create;
#ifdef _WIN32
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
#ifdef _WIN32
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

void ShmInterface::getVersion(int& major, int& minor, int& bugfix) const {
    major = reinterpret_cast<const Header *>(data_)->versionMajor;
    minor = reinterpret_cast<const Header *>(data_)->versionMinor;
    bugfix = reinterpret_cast<const Header *>(data_)->versionBugfix;
}

} // vst
