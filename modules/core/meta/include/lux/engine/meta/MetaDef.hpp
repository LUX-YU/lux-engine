/****************************************************************************************
 * @file   MetaDef.hpp
 * @brief  Compile‑time helpers, fundamental enums and the compact @ref QualType used by
 *         the Lux engine reflection system.
 *
 *         This header is UI‑agnostic and contains **no Blueprint / Pin / Node terms**.
 ****************************************************************************************/
#pragma once
#include <cstdint>
#include <type_traits>
#include <lux/cxx/compile_time/extended_type_traits.hpp>

namespace lux::meta
{
    // TODO change to std::__to_underlying when C++23 is available
    template<typename T> 
    constexpr inline std::underlying_type_t<T> p_to_underlying(T e) noexcept
    {
        return static_cast<std::underlying_type_t<T>>(e);
    }

    /**
     * @enum EBaseType
     * @brief Fundamental category of a C++ type (ignores ref / ptr qualifiers).
     */
    enum class EBaseType : std::uint8_t // total 14 element, actually 4 bits
    {
        Void, 
        // Fundamental start
        Bool = 1,

        Int8, Uint8, Int16, Uint16,
        Int32, Uint32, Int64, Uint64,

        Float, Double = 11, 
        // Fundamental end
        Record, //!< pointer / reference
        Unknown
    };

    /**
     * @enum ETypeQual
     * @brief Reference / pointer qualifier of a @ref QualType.
     */
    enum class ETypeQual : std::uint8_t // total 9 element, actually 4 bits
    {
        Value = 0,     //!< plain value   T
        LRef  = 1,      //!< non‑const l‑value ref  T&
        RRef  = 2,      //!< r‑value ref  T&&
        LRefToConst = 3,  //!< const l‑value ref  const T&
        RRefToConst = 4,  //!< const r‑value ref  const T&&
        Ptr = 5,        //!< pointer  T*
        PtrToConst = 6, //!< const pointer  const T*
        ConstPtr = 7,   ////!< const pointer  T* const
        ConstPtrToConst = 8 //!< const pointer to const  const T* const
    };

    static constexpr inline std::uint8_t p_base_type_num = p_to_underlying(EBaseType::Unknown) + 1; // 4 bits for base type
    static constexpr inline std::uint8_t p_type_qual_num = p_to_underlying(ETypeQual::ConstPtrToConst) + 1; // 4 bits for type qualifier

    /**
     * @struct QualType
     * @brief A 8‑bit POD describing compatibility checks.
     *
     * Packing layout (LSB → MSB):
     * 4 bits – @ref EBaseType
     * 4 bits – @ref ETypeQual
     */
    struct QualType
    {
        std::uint8_t base : 4;
        std::uint8_t qual : 4;
        // ---------------------------------------------------------------------
        constexpr bool operator==(const QualType&) const = default;
    };

    static inline constexpr bool p_is_base_fundamental(const QualType& t) noexcept
    {
        return t.base >= p_to_underlying(EBaseType::Bool) && t.base <= p_to_underlying(EBaseType::Double);
    }

    static inline constexpr bool p_is_base_record(const QualType& t) noexcept
    {
        return t.base == p_to_underlying(EBaseType::Record);
    }

    /**
     * @tparam T  any C++ type
     *
     * Primary template – specialised below for concrete cases.
     */
    template<typename U>
    consteval EBaseType deduce_base()
    {
        using B = std::remove_cvref_t<std::remove_pointer_t<U>>;
        if      constexpr (std::is_void_v<B>)              return EBaseType::Void;
        else if constexpr (std::same_as<B, bool>)          return EBaseType::Bool;
        else if constexpr (std::same_as<B, std::int8_t>)   return EBaseType::Int8;
        else if constexpr (std::same_as<B, std::uint8_t>)  return EBaseType::Uint8;
        else if constexpr (std::same_as<B, std::int16_t>)  return EBaseType::Int16;
        else if constexpr (std::same_as<B, std::uint16_t>) return EBaseType::Uint16;
        else if constexpr (std::same_as<B, std::int32_t>)  return EBaseType::Int32;
        else if constexpr (std::same_as<B, std::uint32_t>) return EBaseType::Uint32;
        else if constexpr (std::same_as<B, std::int64_t>)  return EBaseType::Int64;
        else if constexpr (std::same_as<B, std::uint64_t>) return EBaseType::Uint64;
        else if constexpr (std::same_as<B, float>)         return EBaseType::Float;
        else if constexpr (std::same_as<B, double>)        return EBaseType::Double;
        else if constexpr (std::is_class_v<B>)             return EBaseType::Record;
        else                                               return EBaseType::Unknown;
    }

    //----------- 2.  推导 qualifier ----------------------------------------------------
    template<typename T>
    consteval ETypeQual deduce_qual()
    {
        if constexpr (std::is_pointer_v<T>)
        {
            using Pointee = std::remove_pointer_t<T>;
            constexpr bool ptr_const = std::is_const_v<std::remove_reference_t<T>>;
            constexpr bool obj_const = std::is_const_v<Pointee>;

            if      constexpr (ptr_const && obj_const) return ETypeQual::ConstPtrToConst;
            else if constexpr (ptr_const && !obj_const) return ETypeQual::ConstPtr;
            else if constexpr (!ptr_const && obj_const) return ETypeQual::PtrToConst;
            else                                         return ETypeQual::Ptr;
        }
        else if constexpr (std::is_lvalue_reference_v<T>)
        {
            using Refee = std::remove_reference_t<T>;
            return std::is_const_v<Refee> ? ETypeQual::LRefToConst : ETypeQual::LRef;
        }
        else if constexpr (std::is_rvalue_reference_v<T>)
        {
            using Refee = std::remove_reference_t<T>;
            return std::is_const_v<Refee> ? ETypeQual::RRefToConst : ETypeQual::RRef;
        }
        else
        {
            return ETypeQual::Value;
        }
    }

    template<typename T>
    consteval QualType make_qual_type()
    {
        return {
            static_cast<std::uint8_t>(p_to_underlying(deduce_base<std::remove_cvref_t<T>>())),
            static_cast<std::uint8_t>(p_to_underlying(deduce_qual<T>()))
        };
    }

    // Because our reflection system currently supports only T*, const T, and const T&,
    // this function is already sufficient for our present needs.
    template<class T>
    struct remove_one_modifier
    {
        using type = std::remove_cv_t<
            std::remove_reference_t<  
            std::remove_pointer_t<
            T
        >>>;
    };

    template<class T> using remove_one_modifier_t = typename remove_one_modifier<T>::type;

    /** @brief Produce @ref QualType for a C++ type `T`. */
    template<typename T> constexpr inline QualType __builtin_qual_type = make_qual_type<T>();

    template<typename T>
    constexpr inline QualType* builtin_qual_type_ptr()
    {
        return &__builtin_qual_type<T>;
    }
} // namespace lux::meta
