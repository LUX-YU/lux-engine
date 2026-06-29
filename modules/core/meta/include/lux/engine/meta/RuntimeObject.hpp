/****************************************************************************************
 * @file   RuntimeObject.hpp
 * @brief  Runtime value holder with true SBO (< 8 B) + pointer-tagged heap flag
 ****************************************************************************************/
#pragma once

#include "Meta.hpp"

#include <cassert>
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
        /* 0. 默认构造 —— invalid                                             */
        /* ------------------------------------------------------------------ */
        RuntimeObject() = default;

        /* 禁用拷贝，只保留移动语义 */
        RuntimeObject(const RuntimeObject&) = delete;
        RuntimeObject& operator=(const RuntimeObject&) = delete;

        /* ------------------------------------------------------------------ */
        /* 1. 移动构造                                                        */
        /* ------------------------------------------------------------------ */
        RuntimeObject(RuntimeObject&& other) noexcept
        {
            swap(*this, other);
        }

        /* ------------------------------------------------------------------ */
        /* 2. 移动赋值                                                        */
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
        /* 3. 反射类型：由 RefClass 构造                                         */
        /* ------------------------------------------------------------------ */
        explicit RuntimeObject(const RefClass* cls)
        {
            auto* type_ptr = &cls->type;
            setTagged(type_ptr, true);
            void* p = ::operator new(type_ptr->size, std::align_val_t{ SBO_ALIGN });
            try {
                assert(cls->construct && "RefClass::construct must be valid");
                cls->construct(p);
            }
            catch (...) {
                ::operator delete(p, std::align_val_t{ SBO_ALIGN });
                setTagged(nullptr, false);
                throw;
            }
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
        /* 4. 内建 ≤ 8B 类型：直接放进 SBO（trivially copyable）               */
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
        /* 5. 析构                                                            */
        /* ------------------------------------------------------------------ */
        ~RuntimeObject() noexcept
        {
            cleanup();
        }

        /* ------------------------------------------------------------------ */
        /* 6. 状态 / 访问                                                     */
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
        /* 7. 显式复制                                                        */
        /* ------------------------------------------------------------------ */
        bool copyTo(RuntimeObject& dst)
        {
            return cloneImpl(dst);
        }

        /* ------------------------------------------------------------------ */
        /* 8. 重置 / 交换                                                     */
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
        /* 清理当前持有的资源                                                 */
        /* ------------------------------------------------------------------ */
        void cleanup() noexcept
        {
            auto* type_ptr = getType();
            if (!type_ptr) return;
            if (isHeap()) {
                auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                cls->destruct(storage_.heap);
                ::operator delete(storage_.heap, std::align_val_t{ SBO_ALIGN });
                storage_.heap = nullptr;
            }
            tagged_type_ = 0;
        }

        /* ------------------------------------------------------------------ */
        /* 克隆（强异常安全：先构造临时，再 swap 到 dst）                     */
        /* ------------------------------------------------------------------ */
        bool cloneImpl(RuntimeObject& dst)
        {
            auto* type_ptr = getType();
            if (!type_ptr) return false;
            bool heap = isHeap();

            // 如果类型和存储方式都相同，直接 in-place copy
            if (dst.getType() == type_ptr && dst.isHeap() == heap)
            {
                if (heap) {
                    auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                    if (!cls->copy) // 可能是个空函数
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
                try {
                    auto* cls = static_cast<const RefClass*>(type_ptr->ptr);
                    if (!cls->copy_construct) // 可能是个空函数
                    {
						tmp.setTagged(nullptr, false);
                        return false;
                    }
                    tmp.storage_.heap = ::operator new(type_ptr->size, std::align_val_t{ SBO_ALIGN });
                    cls->copy_construct(tmp.storage_.heap, storage_.heap);
                }
                catch (...) {
                    ::operator delete(tmp.storage_.heap, std::align_val_t{ SBO_ALIGN });
					tmp.setTagged(nullptr, false);
                    throw;
                }
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
