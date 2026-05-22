#ifndef VIG_ALIGNED_ALLOCATOR_HPP
#define VIG_ALIGNED_ALLOCATOR_HPP

#include "esp_heap_caps.h"
#include <limits>
#include <new>
#include <cstdlib>

namespace vig::memory
{

    // @brief Use this to allocate memory in PSRAM with alignment.
    // Use 64-byte alignment for ESP32-P4.
    template <typename T, size_t Alignment = 64>
    struct AlignedPsramAllocator
    {
        static_assert(Alignment % 2 == 0, "Alignment must be a multiple of 2.");

        using value_type = T;

        AlignedPsramAllocator() noexcept = default;

        template <typename U>
        constexpr AlignedPsramAllocator(const AlignedPsramAllocator<U, Alignment> &) noexcept {}

        template <typename U>
        struct rebind
        {
            using other = AlignedPsramAllocator<U, Alignment>;
        };

        [[nodiscard]] T *allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {

#ifdef __cpp_exceptions
                throw std::bad_alloc();
#else
                abort();
#endif
            }

            void *ptr = heap_caps_aligned_alloc(Alignment, n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!ptr)
            {
#ifdef __cpp_exceptions
                throw std::bad_alloc();
#else
                abort();
#endif
            }
            return static_cast<T *>(ptr);
        }

        void deallocate(T *p, std::size_t) noexcept
        {
            heap_caps_free(p);
        }
    };

    template <typename T, size_t AlignA, typename U, size_t AlignB>
    constexpr bool operator==(const AlignedPsramAllocator<T, AlignA> &, const AlignedPsramAllocator<U, AlignB> &) noexcept
    {
        return AlignA == AlignB;
    }

    template <typename T, size_t AlignA, typename U, size_t AlignB>
    constexpr bool operator!=(const AlignedPsramAllocator<T, AlignA> &, const AlignedPsramAllocator<U, AlignB> &) noexcept
    {
        return AlignA != AlignB;
    }

} // namespace vig::memory

#endif // VIG_ALIGNED_ALLOCATOR_HPP
