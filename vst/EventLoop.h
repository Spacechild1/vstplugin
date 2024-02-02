#pragma once

#include "Log.h"
#include "Interface.h"

#include <mutex>

namespace vst {

class BaseEventLoop {
public:
    static constexpr int updateIntervalMillis = 30;

    virtual ~BaseEventLoop() {}

    UIThread::Handle addPollFunction(UIThread::PollFunction fn, void *context) {
        std::unique_lock lock(pollFunctionMutex_);
        auto handle = nextPollFunctionHandle_++;
        pollFunctions_.emplace(handle, [context, fn](){ fn(context); });
        lock.unlock();
        // defer to UI thread!
        UIThread::callAsync([](void *x) {
            static_cast<BaseEventLoop *>(x)->updatePollFunctions();
        }, this);
        return handle;
    }

    void removePollFunction(UIThread::Handle handle) {
        {
            std::lock_guard lock(pollFunctionMutex_);
            pollFunctions_.erase(handle);
        }
        // defer to UI thread!
        UIThread::callAsync([](void *x) {
            static_cast<BaseEventLoop *>(x)->updatePollFunctions();
        }, this);
    }
protected:
    // called by derived classes in poll timer function
    void doPoll() {
        std::lock_guard lock(pollFunctionMutex_);
        for (auto& [_, fn] : pollFunctions_) {
            fn();
        }
    }

    // always called on UI thread!
    virtual void startPolling() = 0;
    // always called on UI thread!
    virtual void stopPolling() = 0;
private:
    void updatePollFunctions() {
        std::unique_lock lock(pollFunctionMutex_);
        bool empty = pollFunctions_.empty();
        lock.unlock();
        // This is called whenever poll functions have been added/removed,
        // so even if a new poll function is added/removed after we have
        // unlocked the mutex, it will eventually do the right thing.
        if (!empty && !isPolling_) {
            LOG_DEBUG("EventLoop: start polling");
            startPolling();
            isPolling_ = true;
        } else if (empty && isPolling_) {
            LOG_DEBUG("EventLoop: stop polling");
            stopPolling();
            isPolling_ = false;
        }
    }

    UIThread::Handle nextPollFunctionHandle_ = 0;
    bool isPolling_ = false;
    std::unordered_map<UIThread::Handle, std::function<void()>> pollFunctions_;
    std::mutex pollFunctionMutex_;
};

} // namespace vst
