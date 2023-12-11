#pragma once

// for VST_HOST_SYSTEM
#include "Interface.h"

#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>
#include <memory>

// NOTE: The Wine host needs to use the shared memory functions
// of the host environment so it can talk to the client.
// In fact, this is the very reason why we have to compile
// the host with wineg++ in the first place (instead of just
// using a regular Windows build).

// Linux: unnamed semaphores don't work for IPC between apps compiled
// for different CPU architectures, so we have to use a futex instead.
// BTW, named semaphores are not reliable, either:
// https://sourceware.org/bugzilla/show_bug.cgi?id=17980
#if VST_HOST_SYSTEM == VST_LINUX
# if 1
#  define SHM_FUTEX 1
# else
#  define SHM_SEMAPHORE 1
# endif
#endif

// Windows: we use unnamed Events and duplicate the handles in the child process.
// This *might* be more efficient than using named Events.
#if VST_HOST_SYSTEM == VST_WINDOWS
# define SHM_EVENT 1
#endif

// macOS: for now we use named Posix semaphores, but we might later switch to Mach semaphores.
// BTW, macOS doesn't support unnamed Posix semaphores at all.
#if VST_HOST_SYSTEM == VST_MACOS
# define SHM_SEMAPHORE 1
#endif

namespace vst {

class ShmInterface;

class ShmChannel {
 public:
    enum Type {
        Queue,
        Request
    };
    // immutable data
    struct Header {
        Header(Type _type, const char *_name, uint32_t _size)
            : size(_size), offset(sizeof(Header)), type(_type) {
            snprintf(name, sizeof(name), "%s", _name);
        }
        uint32_t size;
        uint32_t offset; // = sizeof(Header) = 128 resp. 64
        uint32_t type;
        char name[20];
    #if SHM_FUTEX
        // atomic integers for Futex
        std::atomic<uint32_t> data1{0};
        std::atomic<uint32_t> data2{0};
        char padding[24];
    #elif SHM_EVENT
        // Event handles
        uint32_t data1{0};
        uint32_t data2{0};
        char padding[24];
    #elif SHM_SEMAPHORE
        // semaphore names
        char data1[32];
        char data2[32];
        char padding[32];
    #endif
    };
    // mutable data
    struct Data {
        Data() = default;
        uint32_t capacity = 0;
        std::atomic<uint32_t> size{0};
        char data[1]; // flexible array
    };
    // raw message
    struct Message {
        static const size_t alignment = 8;

        uint32_t size;
        char data[1];
    };

    static const size_t alignment = 64;

    ShmChannel() = default;
    ShmChannel(Type type, int32_t size,
               const std::string& name);
    ShmChannel(const ShmChannel&) = delete;
    ShmChannel(ShmChannel&&) = default;
    ~ShmChannel();
    ShmChannel& operator=(const ShmChannel&) = delete;
    ShmChannel& operator=(ShmChannel&&) = default;

    Type type() const { return type_; }
    int32_t size() const { return totalSize_; }
    int32_t capacity() const { return data_->capacity; }
    const std::string& name() const { return name_; }

    size_t peekMessage() const;
    // read queue message (thread-safe, copy)
    bool readMessage(void * buffer, size_t& size);
    // write queue message
    bool writeMessage(const void *data, size_t size);

    // add request message (not thread-safe)
    bool addMessage(const void * data, size_t size);
    // get request message (not thread-safe, no copy)
    bool getMessage(const void *& data, size_t& size);

    void clear();
    void reset();

    void post();
    void wait();
    void postReply();
    void waitReply();

    void init(ShmInterface& shm, char *data, int num);
 private:
    struct HandleDeleter { void operator()(void *); };
    using Handle = std::unique_ptr<void, HandleDeleter>;

    bool owner_ = false;
    Type type_ = Queue;
    int32_t totalSize_ = 0;
    int32_t bufferSize_ = 0;
    std::string name_;
    Handle eventA_;
    Handle eventB_;
    Header *header_ = nullptr;
    Data *data_ = nullptr;
    uint32_t rdhead_ = 0;
    uint32_t wrhead_ = 0;
    // helper methods
    void initEvent(ShmInterface& shm, Handle& event, void *data);
    void postEvent(void *event);
    void waitEvent(void *event);
};

class ShmInterface {
 public:
    static const int32_t maxNumChannels = 60;

    struct Header {
        Header(uint32_t _size, uint32_t _numChannels);

        uint32_t size;
        uint8_t versionMajor;
        uint8_t versionMinor;
        uint8_t versionPatch;
        uint8_t unused;
    #if SHM_EVENT
        int32_t processID;
    #else
        uint32_t reserved;
    #endif
        uint32_t numChannels;
        uint32_t channelOffset[maxNumChannels];
    };

    // SharedMemory();
    ShmInterface();
    ~ShmInterface();
    ShmInterface(const ShmInterface&) = delete;
    ShmInterface(ShmInterface&&) = default;
    ShmInterface& operator=(const ShmInterface&) = delete;
    ShmInterface& operator=(ShmInterface&&) = default;

    // connect to existing shared memory interface
    void connect(const std::string& path);
    void disconnect();

    // create shared memory interface
    void addChannel(ShmChannel::Type type,
                    size_t size, const std::string& name);
    void create();
    void close();

    const std::string& path() const { return path_; }
    size_t size() const { return size_; }

    void setOrphaned() {
        orphaned_.store(true);
    }

    // access channels
    int numChannels() const {
        return channels_.size();
    }
    ShmChannel& getChannel(int i){
        return channels_[i];
    }
    const ShmChannel& getChannel(int i) const {
        return channels_[i];
    }

    void getVersion(int& major, int& minor, int& patch) const;

#if SHM_EVENT
    void * getParentProcessHandle() const { return hParentProcess_; }
#endif
 private:
    std::vector<ShmChannel> channels_;
    bool owner_ = false;
    std::atomic<bool> orphaned_{false};
    std::string path_;
#if VST_HOST_SYSTEM == VST_WINDOWS
    void *hMapFile_ = nullptr;
#endif
#if SHM_EVENT
    void *hParentProcess_ = nullptr;
#endif
    size_t size_ = 0;
    char *data_ = nullptr;

    void openShm(const std::string& path, bool create);
    void closeShm();
};


} // vst
