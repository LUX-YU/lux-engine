/****************************************************************************************
 * @file   RuntimeObject.hpp
 * @brief  Runtime value holder with 16-byte SBO + pointer-tagged heap flag
 ****************************************************************************************/
#pragma once

#include "Meta.hpp"
#include <exception>

#include <lux/cxx/compile_time/expected.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace lux::meta
{
    enum class ERuntimeObjectError : std::uint8_t
    {
        INVALID_TYPE,
        CONSTRUCTION_UNAVAILABLE,
        CONSTRUCTION_FAILURE,
        DEFAULT_UNAVAILABLE,
        COPY_UNAVAILABLE,
        COPY_FAILURE,
    };

    class RuntimeObject
    {
        /* ------------------------------------------------------------------ */
        /*  compile-time config                                               */
        /* ------------------------------------------------------------------ */
        static constexpr std::size_t SBO_SIZE = 16; // ≤ 16 B
        static constexpr std::size_t SBO_ALIGN = alignof(std::max_align_t);

        union alignas(SBO_ALIGN) Storage
        {
            void* heap;
            std::byte sbo[SBO_SIZE];
        };

        // TaggedPtr: store type pointer and heap flag in low bit
        static constexpr uintptr_t HEAP_BIT = 1;
        uintptr_t tagged_type_ = 0;
        Storage storage_{};

    public:
        /* ------------------------------------------------------------------ */
        /* 0. Default construction — legal empty value                         */
        /* ------------------------------------------------------------------ */
        RuntimeObject() noexcept = default;

        /* Copying is disabled; only move semantics are supported */
        RuntimeObject(const RuntimeObject&) = delete;
        RuntimeObject& operator=(const RuntimeObject&) = delete;

        /* ------------------------------------------------------------------ */
        /* 1. Move constructor                                                */
        /* ------------------------------------------------------------------ */
        RuntimeObject(RuntimeObject&& other) noexcept
        {
            swap(*this, other);
        }

        /* ------------------------------------------------------------------ */
        /* 2. Move assignment                                                 */
        /* ------------------------------------------------------------------ */
        RuntimeObject& operator=(RuntimeObject&& other) noexcept
        {
            if (this != &other)
            {
                cleanup();
                swap(*this, other);
            }
            return *this;
        }

        /* ------------------------------------------------------------------ */
        /* 3. Reflected type: constructed from a RefClass                     */
        /* ------------------------------------------------------------------ */
        [[nodiscard]] static lux::cxx::expected<RuntimeObject, ERuntimeObjectError> create(const RefClass* cls) noexcept
        {
            if (cls == nullptr || !validHeapType(cls->type))
            {
                return lux::cxx::unexpected<ERuntimeObjectError>(ERuntimeObjectError::INVALID_TYPE);
            }
            if (!cls->construct || !cls->destruct)
            {
                return lux::cxx::unexpected<ERuntimeObjectError>(ERuntimeObjectError::CONSTRUCTION_UNAVAILABLE);
            }

            RuntimeObject result;
            void* storage = allocate(cls->type);
            try
            {
                cls->construct(storage);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                deallocate(storage, cls->type);
                return lux::cxx::unexpected<ERuntimeObjectError>(ERuntimeObjectError::CONSTRUCTION_FAILURE);
            }
            result.setTagged(&cls->type, true);
            result.storage_.heap = storage;
            return result;
        }

        // special support for std::string
        [[nodiscard]] static lux::cxx::expected<RuntimeObject, ERuntimeObjectError> create(std::string value) noexcept
        {
            if (!ReflectionRegistry::initialized())
            {
                return lux::cxx::unexpected(ERuntimeObjectError::INVALID_TYPE);
            }
            const auto* string_class_meta = ReflectionRegistry::instance().findClass("std::string");
            const bool is_invalid_metadata = string_class_meta == nullptr || !validHeapType(string_class_meta->type);
            const bool is_layout_mismatch =
                !is_invalid_metadata && (string_class_meta->type.size != sizeof(std::string) ||
                                         string_class_meta->type.alignment != alignof(std::string));
            if (is_invalid_metadata || is_layout_mismatch)
            {
                return lux::cxx::unexpected<ERuntimeObjectError>(ERuntimeObjectError::INVALID_TYPE);
            }

            RuntimeObject result;
            void* storage = allocate(string_class_meta->type);
            new (storage) std::string(std::move(value));
            result.setTagged(&string_class_meta->type, true);
            result.storage_.heap = storage;
            return result;
        }

        /* ------------------------------------------------------------------ */
        /* 4. Built-in types ≤ 16 B: stored directly in the SBO (trivially copyable) */
        /* ------------------------------------------------------------------ */
        template <typename T, typename U = std::decay_t<T>>
            requires(sizeof(U) <= SBO_SIZE && std::is_trivially_copyable_v<U> && alignof(U) <= SBO_ALIGN)
        explicit RuntimeObject(T&& v) noexcept
        {
            auto* type_ptr = builtin_ref_type_ptr<U>();
            setTagged(type_ptr, false);
            new (storage_.sbo) U(std::forward<T>(v));
        }

        /// Produces a zero-filled trivial value. Non-trivial values require create(RefClass*).
        /// The metadata (and its registry/code owner) must outlive this value and its clones.
        [[nodiscard]] static lux::cxx::expected<RuntimeObject, ERuntimeObjectError>
        defaultOf(const RefType& type) noexcept
        {
            if (!validHeapType(type))
            {
                return lux::cxx::unexpected(ERuntimeObjectError::INVALID_TYPE);
            }
            if (!type.traits.is_trivially_copyable)
            {
                return lux::cxx::unexpected(ERuntimeObjectError::DEFAULT_UNAVAILABLE);
            }

            RuntimeObject result;
            const bool heap = !fitsSbo(type);
            if (heap)
            {
                result.storage_.heap = allocate(type);
            }
            result.setTagged(&type, heap);
            std::memset(result.data(), 0, type.size);
            return result;
        }

        /* ------------------------------------------------------------------ */
        /* 5. Destructor                                                      */
        /* ------------------------------------------------------------------ */
        ~RuntimeObject() noexcept
        {
            cleanup();
        }

        /* ------------------------------------------------------------------ */
        /* 6. State / access                                                  */
        /* ------------------------------------------------------------------ */
        [[nodiscard]] bool isValid() const noexcept
        {
            return getType() != nullptr;
        }

        [[nodiscard]] const RefType* type() const noexcept
        {
            return getType();
        }

        [[nodiscard]] void* data() noexcept
        {
            return isHeap() ? storage_.heap : storage_.sbo;
        }

        [[nodiscard]] const void* data() const noexcept
        {
            return isHeap() ? storage_.heap : storage_.sbo;
        }

        template <typename T> [[nodiscard]] T& get() & noexcept
        {
            assert(match<T>());
            return *std::launder(reinterpret_cast<T*>(data()));
        }

        template <typename T> [[nodiscard]] const T& get() const& noexcept
        {
            assert(match<T>());
            return *std::launder(reinterpret_cast<const T*>(data()));
        }

        /* ------------------------------------------------------------------ */
        /* 7. Explicit copy                                                   */
        /* ------------------------------------------------------------------ */
        /// Cloning empty succeeds with empty. Failure never changes the source.
        [[nodiscard]] lux::cxx::expected<RuntimeObject, ERuntimeObjectError> clone() const noexcept
        {
            RuntimeObject result;
            const auto* value_type = getType();
            if (!value_type)
            {
                return result;
            }

            const bool heap = isHeap();
            if (!heap)
            {
                std::memcpy(result.storage_.sbo, storage_.sbo, value_type->size);
                result.setTagged(value_type, false);
                return result;
            }

            const RefClass* cls = nullptr;
            if (!value_type->traits.is_trivially_copyable)
            {
                cls = static_cast<const RefClass*>(value_type->ptr);
                const bool is_copy_unavailable = cls == nullptr || !cls->copy_construct;
                if (is_copy_unavailable)
                {
                    return lux::cxx::unexpected(ERuntimeObjectError::COPY_UNAVAILABLE);
                }
            }
            void* storage = allocate(*value_type);
            if (cls)
            {
                try
                {
                    cls->copy_construct(storage, data());
                }
                catch (const std::bad_alloc&)
                {
                    std::terminate();
                }
                catch (...)
                {
                    deallocate(storage, *value_type);
                    return lux::cxx::unexpected(ERuntimeObjectError::COPY_FAILURE);
                }
            }
            else
            {
                std::memcpy(storage, data(), value_type->size);
            }
            result.storage_.heap = storage;
            result.setTagged(value_type, true);
            return result;
        }

        /* ------------------------------------------------------------------ */
        /* 8. Reset / swap                                                    */
        /* ------------------------------------------------------------------ */
        void reset() noexcept
        {
            cleanup();
        }

        friend void swap(RuntimeObject& a, RuntimeObject& b) noexcept
        {
            using std::swap;
            swap(a.tagged_type_, b.tagged_type_);
            swap(a.storage_, b.storage_);
        }

        explicit operator bool() const noexcept
        {
            return isValid();
        }

    private:
        [[nodiscard]] static bool validAlignment(std::size_t alignment) noexcept
        {
            return alignment != 0U && (alignment & (alignment - 1U)) == 0U;
        }

        [[nodiscard]] static bool validHeapType(const RefType& type) noexcept
        {
            return type.size != 0U && validAlignment(type.alignment);
        }

        [[nodiscard]] static std::size_t allocationAlignment(const RefType& type) noexcept
        {
            return type.alignment > SBO_ALIGN ? type.alignment : SBO_ALIGN;
        }

        [[nodiscard]] static bool fitsSbo(const RefType& type) noexcept
        {
            return type.size <= SBO_SIZE && type.alignment <= SBO_ALIGN;
        }

        [[nodiscard]] static void* allocate(const RefType& type) noexcept
        {
            if (!validHeapType(type))
            {
                std::terminate();
            }
            return ::operator new(type.size, std::align_val_t{allocationAlignment(type)});
        }

        static void deallocate(void* storage, const RefType& type) noexcept
        {
            ::operator delete(storage, std::align_val_t{allocationAlignment(type)});
        }

        [[nodiscard]] const RefType* getType() const noexcept
        {
            return reinterpret_cast<const RefType*>(tagged_type_ & ~HEAP_BIT);
        }

        [[nodiscard]] bool isHeap() const noexcept
        {
            return (tagged_type_ & HEAP_BIT) != 0;
        }

        void setTagged(const RefType* type_ptr, bool heap) noexcept
        {
            static_assert(alignof(RefType) > 1);
            uintptr_t u = reinterpret_cast<uintptr_t>(type_ptr);
            assert((u & HEAP_BIT) == 0 && "Type pointer not sufficiently aligned for tagging");
            tagged_type_ = u | (heap ? HEAP_BIT : 0);
        }

        /* ------------------------------------------------------------------ */
        /* Release whatever resource is currently held                       */
        /* ------------------------------------------------------------------ */
        void cleanup() noexcept
        {
            auto* type_ptr = getType();
            if (!type_ptr)
            {
                return;
            }
            if (isHeap())
            {
                // Trivially-copyable payloads (defaultOf's heap path) have
                // no destructor to run — and may not even carry a RefClass.
                if (!type_ptr->traits.is_trivially_copyable)
                {
                    auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                    cls->destruct(storage_.heap);
                }
                deallocate(storage_.heap, *type_ptr);
                storage_.heap = nullptr;
            }
            tagged_type_ = 0;
        }

        template <typename T> [[nodiscard]] bool match() const noexcept
        {
            auto* type_ptr = getType();
            return type_ptr && type_ptr->hash == lux::cxx::type_hash<T>() && type_ptr == builtin_ref_type_ptr<T>();
        }
    };

} // namespace lux::meta
