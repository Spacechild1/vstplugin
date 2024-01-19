#pragma once

#include "Interface.h"

namespace vst {

struct Bus : AudioBus {
    Bus() {
        numChannels = 0;
        channelData32 = nullptr;
    }
    Bus(int n) {
        numChannels = n;
        if (n > 0){
            channelData32 = new float *[n];
        } else {
            channelData32 = nullptr;
        }
    }
    ~Bus(){
        if (channelData32){
            delete[] (float **)channelData32;
        }
    }

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    Bus(Bus&& other) noexcept  {
        numChannels = other.numChannels;
        channelData32 = other.channelData32;
        other.numChannels = 0;
        other.channelData32 = nullptr;
    }
    Bus& operator=(Bus&& other) noexcept {
        numChannels = other.numChannels;
        channelData32 = other.channelData32;
        other.numChannels = 0;
        other.channelData32 = nullptr;
        return *this;
    }
};

} // vst
