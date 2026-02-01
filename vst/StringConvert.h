#pragma once

#include "pluginterfaces/vst/vsttypes.h"

#include <string>
#include <string_view>
#include <cstdint>
#include <optional>

namespace vst {
namespace StringConvert {

// from UTF-8 to UTF-16
bool convert(std::string_view utf8Str, Steinberg::Vst::String128 str);

bool convert(std::string_view utf8Str, Steinberg::Vst::TChar* str, size_t maxSize);

// from UTF-16 to UTF-8
std::optional<std::string> convert(const Steinberg::Vst::TChar* str);

} // StringConvert
} // vst
