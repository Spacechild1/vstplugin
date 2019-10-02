#pragma once

#include <type_traits>
#include <memory>
#include "SC_InterfaceTable.h"

#ifndef DEBUG_RT_MEMORY
#define DEBUG_RT_MEMORY 0
#endif

namespace rt {

    extern InterfaceTable* interfaceTable;

    template <class T>
    class allocator {
    public:
        using value_type = T;

        allocator(World *world) noexcept 
            : world_(world) {}

        template <typename U> 
        allocator(allocator<U> const& other) noexcept 
            : world_(other.world_) {}

        value_type* allocate(std::size_t n) {
            auto p = static_cast<value_type*>(interfaceTable->fRTAlloc(world_, n * sizeof(T)));
        #if DEBUG_RT_MEMORY
            interfaceTable->fPrint("allocate %d bytes at %p\n", n * sizeof(T), p);
        #endif
            return p;
        }

        void deallocate(value_type* p, std::size_t n) noexcept {
        #if DEBUG_RT_MEMORY
            interfaceTable->fPrint("deallocate %d bytes at %p\n", n * sizeof(T), p);
        #endif
            interfaceTable->fRTFree(world_, p);
        }

        World* world_; // must be public...
    };

    template <class T, class U>
    bool
        operator==(allocator<T> const&, allocator<U> const&) noexcept
    {
        return true;
    }

    template <class T, class U>
    bool
        operator!=(allocator<T> const& x, allocator<U> const& y) noexcept
    {
        return !(x == y);
    }

    template<typename T>
    class deleter {
    public:
        deleter(World* world)
            : world_(world) {}
        void operator()(T* ptr) {
            ptr->~T();
            interfaceTable->fRTFree(world_, ptr);
        }
    private:
        World* world_;
    };

    template<typename T>
    using shared_ptr = std::shared_ptr<T>;

    template<typename T, typename... Args>
    shared_ptr<T> make_shared(World* world, Args&&... args) {
        auto obj = (T*)interfaceTable->fRTAlloc(world, sizeof(T));
        if (obj) {
            ::new (obj) T(std::forward<Args>(args)...);
        }
        return shared_ptr<T>(obj, deleter<T>(world), allocator<T>(world));
    };

} // rt
