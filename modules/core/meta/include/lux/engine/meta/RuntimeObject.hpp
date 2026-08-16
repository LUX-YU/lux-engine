/****************************************************************************************
 * @file   RuntimeObject.hpp
 * @brief  Runtime value holder with true SBO (< 8 B) + pointer-tagged heap flag
 ****************************************************************************************/
#pragma once

#include "Meta.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>
#include <cstdint>

namespace lux::meta
{
    class RuntimeObject
    {
        /* ------------------------------------------------------------------ */
        /*  compile-time config                                               */
        /* ------------------------------------------------------------------ */
        static constexpr std::size_t SBO_SIZE  = 16;                     // ≤ 16 B
        static constexpr std::size_t SBO_ALIGN = alignof(std::max_align_t);

        union alignas(SBO_ALIGN) Storage
        {
            void* heap;
            std::byte sbo[SBO_SIZE];
        };

        // TaggedPtr: store type pointer and heap flag in low bit
        static constexpr uintptr_t HEAP_BIT = 1;
        uintptr_t tagged_type_ = 0;
        Storage   storage_{};

    public:
        /* ------------------------------------------------------------------ */
        /* 0. Default construction — invalid                                  */
        /* ------------------------------------------------------------------ */
        RuntimeObject() = default;

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
            if (this != &other) {
                cleanup();
                swap(*this, other);
            }
            return *this;
        }

        /* ------------------------------------------------------------------ */
        /* 3. Reflected type: constructed from a RefClass                     */
        /* ------------------------------------------------------------------ */
        explicit RuntimeObject(const RefClass* cls)
        {
            auto* type_ptr = &cls->type;
            setTagged(type_ptr, true);
            if (!cls->construct)
                std::abort();
            void* p = ::operator new(
                type_ptr->size,
                std::align_val_t{SBO_ALIGN},
                std::nothrow
            );
            if (!p)
                std::abort();
            cls->construct(p);
            storage_.heap = p;
        }

		// special support for std::string
		explicit RuntimeObject(std::string str)
		{
            static auto* string_class_meta = ReflectionRegistry::instance().findClass("std::string");
			setTagged(&string_class_meta->type, true);
			void* p = ::operator new(sizeof(std::string), std::align_val_t{SBO_ALIGN});
            new (p) std::string(std::move(str));
			storage_.heap = p;
		}

		// special support for std::string_view
        explicit RuntimeObject(std::string_view str)
        {
			static_assert(sizeof(std::string_view) <= SBO_SIZE, "std::string_view size exceeds SBO_SIZE");
			static auto* string_class_meta = ReflectionRegistry::instance().findClass("std::string_view");
			setTagged(&string_class_meta->type, false);
			new (storage_.sbo) std::string_view(str);
        }

        /* ------------------------------------------------------------------ */
        /* 4. Built-in types ≤ 8 B: stored directly in the SBO (trivially copyable) */
        /* ------------------------------------------------------------------ */
        template<typename T, typename U = std::decay_t<T>>
            requires(sizeof(U) <= SBO_SIZE && std::is_trivially_copyable_v<U> && alignof(U) <= SBO_ALIGN)
        explicit RuntimeObject(T&& v) noexcept
        {
            auto* type_ptr = builtin_ref_type_ptr<U>();
            setTagged(type_ptr, false);
            new (storage_.sbo) U(std::forward<T>(v));
        }

        /* ------------------------------------------------------------------ */
        /* 4b. Named factory: produces a zero-initialized default value for a
         *     given RefType. Note this can't be expressed via the
         *     RuntimeObject(const RefType*) constructor — that constructor
         *     would match the SBO template above instead, storing the pointer
         *     itself as the value. Non-trivially-copyable types (which need
         *     their constructor to run) return an invalid object; callers
         *     must construct through RefClass::construct instead.            */
        /* ------------------------------------------------------------------ */
        static RuntimeObject defaultOf(const RefType* type)
        {
            RuntimeObject obj;
            if (!type || type->size == 0 || !type->traits.is_trivially_copyable)
                return obj;

            if (type->size <= SBO_SIZE)
            {
                obj.setTagged(type, false);
                std::memset(obj.storage_.sbo, 0, type->size);
            }
            else
            {
                void* p = ::operator new(type->size, std::align_val_t{ SBO_ALIGN });
                std::memset(p, 0, type->size);
                obj.setTagged(type, true);
                obj.storage_.heap = p;
            }
            return obj;
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
        [[nodiscard]] bool           isValid() const noexcept { return getType() != nullptr; }
        [[nodiscard]] const RefType* type()    const noexcept { return getType(); }

        [[nodiscard]] void*          data()       noexcept { return isHeap() ? storage_.heap : storage_.sbo; }
        [[nodiscard]] const void*    data() const noexcept { return isHeap() ? storage_.heap : storage_.sbo; }

        template<typename T>
        [[nodiscard]] T& get() & noexcept
        {
            assert(match<T>());
            return *std::launder(reinterpret_cast<T*>(data()));
        }

        template<typename T>
        [[nodiscard]] const T& get() const& noexcept
        {
            assert(match<T>());
            return *std::launder(reinterpret_cast<const T*>(data()));
        }

        /* ------------------------------------------------------------------ */
        /* 7. Explicit copy                                                   */
        /* ------------------------------------------------------------------ */
        bool copyTo(RuntimeObject& dst)
        {
            return cloneImpl(dst);
        }

        /* ------------------------------------------------------------------ */
        /* 8. Reset / swap                                                    */
        /* ------------------------------------------------------------------ */
        void reset() noexcept { cleanup(); }

        friend void swap(RuntimeObject& a, RuntimeObject& b) noexcept
        {
            using std::swap;
            swap(a.tagged_type_, b.tagged_type_);
            swap(a.storage_, b.storage_);
        }

        explicit operator bool() const noexcept { return isValid(); }

    private:
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
            if (!type_ptr) return;
            if (isHeap()) {
                // Trivially-copyable payloads (defaultOf's heap path) have
                // no destructor to run — and may not even carry a RefClass.
                if (!type_ptr->traits.is_trivially_copyable) {
                    auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                    cls->destruct(storage_.heap);
                }
                ::operator delete(storage_.heap, std::align_val_t{ SBO_ALIGN });
                storage_.heap = nullptr;
            }
            tagged_type_ = 0;
        }

        /* ------------------------------------------------------------------ */
        /* Clone (strong exception safety: build a temporary first, then swap it into dst) */
        /* ------------------------------------------------------------------ */
        bool cloneImpl(RuntimeObject& dst)
        {
            auto* type_ptr = getType();
            if (!type_ptr) return false;
            bool heap = isHeap();

            // Same type and same storage mode — copy in place directly
            if (dst.getType() == type_ptr && dst.isHeap() == heap)
            {
                if (heap) {
                    auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                    if (!cls->copy) // may be a null function pointer
                    {
                        return false;
                    }

                    cls->copy(dst.storage_.heap, storage_.heap);
                }
                else
                {
                    // stack
                    std::memcpy(dst.storage_.sbo, storage_.sbo, SBO_SIZE);
                }
                return true;
            }

            /// Otherwise allocate/construct new copy in temporary object first
            RuntimeObject tmp;
            if (heap)
            {
                auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                if (!cls->copy_construct)
                    return false;
                tmp.storage_.heap = ::operator new(
                    type_ptr->size,
                    std::align_val_t{SBO_ALIGN},
                    std::nothrow
                );
                if (!tmp.storage_.heap)
                    return false;
                cls->copy_construct(tmp.storage_.heap, storage_.heap);
            }
            else
            {
                std::memcpy(tmp.storage_.sbo, storage_.sbo, SBO_SIZE);
            }
            tmp.setTagged(type_ptr, heap);
            swap(tmp, dst);
            return true;
        }

        template<typename T>
        [[nodiscard]] bool match() const noexcept
        {
            auto* type_ptr = getType();
            return type_ptr
                && type_ptr->hash == lux::cxx::type_hash<T>()
                && type_ptr == builtin_ref_type_ptr<T>();
        }
    };

} // namespace lux::meta
