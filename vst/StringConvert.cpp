#include "StringConvert.h"
#include "Interface.h"
#include "Log.h"

#include <codecvt>
#include <locale>

namespace vst {
namespace StringConvert {

namespace {

// On Wine std::wstring_convert would throw an exception
// when using wchar_t, although it has the same size as char16_t.
// Maybe because of some template specialization? Dunno...
#if defined(_WIN32) && !defined(__WINE__)
using unichar = wchar_t;
static_assert(sizeof(wchar_t) == sizeof(char16_t), "bad size for wchar_t!");
#else
using unichar = char16_t;
#endif

using Converter = std::wstring_convert<std::codecvt_utf8_utf16<unichar>, unichar>;

Converter& getConverter() {
#if defined(__MINGW32__) && !defined(__MINGW64__)
    // because of a mingw32 bug, destructors of thread_local STL objects segfault...
    thread_local auto conv = new Converter;
    return *conv;
#else
    thread_local Converter conv;
    return conv;
#endif
}

} // namespace

// from UTF-8 to UTF-16
bool convert(std::string_view utf8Str, Steinberg::Vst::String128 str) {
    return convert(utf8Str, str, 128);
}

bool convert(std::string_view utf8Str, Steinberg::Vst::TChar* str, size_t maxSize) {
    if (utf8Str.size() >= maxSize) {
        return false;
    }
    try {
        auto wstr = getConverter().from_bytes(
            utf8Str.data(), utf8Str.data() + utf8Str.size());
        int n = wstr.size() + 1;
        for (int i = 0; i < n; ++i){
            str[i] = wstr[i];
        }
        return true;
    } catch (const std::range_error& e){
        LOG_ERROR("StringConvert::convert() failed: " << + e.what());
        return false;
    }
}

// from UTF-16 to UTF-8
std::optional<std::string> convert(const Steinberg::Vst::TChar* str) {
    try {
        return getConverter().to_bytes(reinterpret_cast<const unichar *>(str));
    } catch (const std::range_error& e) {
        LOG_ERROR("StringConvert::convert() failed: " << + e.what());
        return std::nullopt;
    }
}

} // StringConvert
} // vst
