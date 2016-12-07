#pragma once

#include <3ds/allocator/linear.h>

template<class T>
class LinearHeapAllocator {
public:
    typedef size_t    size_type;
    typedef ptrdiff_t difference_type;
    typedef T*        pointer;
    typedef const T*  const_pointer;
    typedef T&        reference;
    typedef const T&  const_reference;
    typedef T         value_type;

    template <class U>
    struct rebind { typedef LinearHeapAllocator<U> other; };

    LinearHeapAllocator() throw() {};

    template <class U>
    LinearHeapAllocator(const LinearHeapAllocator<U>&) {};

    template <class U>
    LinearHeapAllocator& operator=(const LinearHeapAllocator<U>&) { return *this; }

    ~LinearHeapAllocator() {}

    pointer allocate(size_type n, const_pointer hint = nullptr) {
        return static_cast<pointer>(linearAlloc(sizeof(T) * n));
    }

    void deallocate(pointer ptr, size_type) {
        if (ptr) {
            linearFree(ptr);
        }
    }
};