#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <atomic>
#include <array>
#include <memory>

namespace vst {

class ShmInterface;

class ShmChannel {
 public:
    // immutable data
    struct Header {
        uint32_t size;
        uint32_t offset; // = sizeof(Header) = 128
        uint32_t type;
        char name[20];
        // holds event/semaphore names (Windows/macOS)
        // or semaphore objects (Linux)
        char event1[32];
        char event2[32];
        char reserved[32];
    };
    // mutable data
    struct Data {
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

    enum Type {
        Queue,
        Request
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
    const std::string& name() const { return name_; }

    size_t peekMessage() const;
    // read queue message (thread-safe, copy)
    bool readMessage(char * buffer, size_t& size);
    // write queue message
    bool writeMessage(const char *data, size_t size);

    // add request message (not thread-safe)
    bool addMessage(const char * data, size_t size);
    // get request message (not thread-safe, no copy)
    bool getMessage(const char *& data, size_t& size);

    void clear();

    void post();
    void wait();
    void postReply();
    void waitReply();

    void init(char *data, ShmInterface& shm, int num);
 private:
    struct HandleDeleter { void operator()(void *); };
    using Handle = std::unique_ptr<void, HandleDeleter>;

    bool owner_ = false;
    Type type_ = Queue;
    int32_t totalSize_ = 0;
    int32_t bufferSize_ = 0;
    std::string name_;
    std::array<Handle, 2> events_;
    Header *header_ = nullptr;
    Data *data_ = nullptr;
    uint32_t rdhead_ = 0;
    uint32_t wrhead_ = 0;
    // helper methods
    void initEvent(int which, const char *data);
    void postEvent(int which);
    void waitEvent(int which);
};

class ShmInterface {
 public:
    static const int32_t maxNumChannels = 60;

    struct Header {
        uint32_t size;
        uint8_t versionMajor;
        uint8_t versionMinor;
        uint8_t versionBugfix;
        uint8_t unused;
        uint32_t reserved;
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
                    const size_t size, const std::string& name);
    void create();
    void close();

    const std::string& path() const { return path_; }
    size_t size() const { return size_; }

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
 private:
    std::vector<ShmChannel> channels_;
    bool owner_ = false;
    std::string path_;
#ifdef _WIN32
    void *hMapFile_ = nullptr;
#endif
    size_t size_ = 0;
    char *data_ = nullptr;

    void openShm(const std::string& path, bool create);
    void closeShm();
};


} // vst
