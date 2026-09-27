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
    /// Opt-in for externally reflected value records which can appear as
    /// nested tagged properties. Records marked in the current generator unit
    /// are recognized directly from generator-owned record identity.
    template <class T> inline constexpr bool is_reflected_value_v = false;

    // TODO change to std::__to_underlying when C++23 is available
    template <typename T> constexpr inline std::underlying_type_t<T> p_to_underlying(T e) noexcept
    {
        return static_cast<std::underlying_type_t<T>>(e);
    }

    /**
     * @enum EBaseType
     * @brief Fundamental category of a C++ type (ignores ref / ptr qualifiers).
     */
    enum class EBaseType : std::uint8_t // total 14 element, actually 4 bits
    {
        VOID,
        // Fundamental start
        BOOL = 1,

        INT8,
        UINT8,
        INT16,
        UINT16,
        INT32,
        UINT32,
        INT64,
        UINT64,

        FLOAT,
        DOUBLE = 11,
        // Fundamental end
        RECORD, //!< pointer / reference
        UNKNOWN
    };

    /**
     * @enum ETypeQual
     * @brief Reference / pointer qualifier of a @ref QualType.
     */
    enum class ETypeQual : std::uint8_t // total 9 element, actually 4 bits
    {
        VALUE = 0,          //!< plain value   T
        L_REF = 1,           //!< non‑const l‑value ref  T&
        R_REF = 2,           //!< r‑value ref  T&&
        L_REF_TO_CONST = 3,    //!< const l‑value ref  const T&
        R_REF_TO_CONST = 4,    //!< const r‑value ref  const T&&
        PTR = 5,            //!< pointer  T*
        PTR_TO_CONST = 6,     //!< const pointer  const T*
        CONST_PTR = 7,       ////!< const pointer  T* const
        CONST_PTR_TO_CONST = 8 //!< const pointer to const  const T* const
    };

    static constexpr inline std::uint8_t p_base_type_num =
        p_to_underlying(EBaseType::UNKNOWN) + 1; // 4 bits for base type
    static constexpr inline std::uint8_t p_type_qual_num =
        p_to_underlying(ETypeQual::CONST_PTR_TO_CONST) + 1; // 4 bits for type qualifier

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
        return t.base >= p_to_underlying(EBaseType::BOOL) && t.base <= p_to_underlying(EBaseType::DOUBLE);
    }

    static inline constexpr bool p_is_base_record(const QualType& t) noexcept
    {
        return t.base == p_to_underlying(EBaseType::RECORD);
    }

    /**
     * @tparam T  any C++ type
     *
     * Primary template – specialised below for concrete cases.
     */
    template <typename U> consteval EBaseType deduce_base()
    {
        using B = std::remove_cvref_t<std::remove_pointer_t<U>>;
        if constexpr (std::is_void_v<B>)
            return EBaseType::VOID;
        else if constexpr (std::same_as<B, bool>)
            return EBaseType::BOOL;
        else if constexpr (std::same_as<B, std::int8_t>)
            return EBaseType::INT8;
        else if constexpr (std::same_as<B, std::uint8_t>)
            return EBaseType::UINT8;
        else if constexpr (std::same_as<B, std::int16_t>)
            return EBaseType::INT16;
        else if constexpr (std::same_as<B, std::uint16_t>)
            return EBaseType::UINT16;
        else if constexpr (std::same_as<B, std::int32_t>)
            return EBaseType::INT32;
        else if constexpr (std::same_as<B, std::uint32_t>)
            return EBaseType::UINT32;
        else if constexpr (std::same_as<B, std::int64_t>)
            return EBaseType::INT64;
        else if constexpr (std::same_as<B, std::uint64_t>)
            return EBaseType::UINT64;
        else if constexpr (std::same_as<B, float>)
            return EBaseType::FLOAT;
        else if constexpr (std::same_as<B, double>)
            return EBaseType::DOUBLE;
        else if constexpr (std::is_enum_v<B>)
            return deduce_base<std::underlying_type_t<B>>();
        else if constexpr (std::is_class_v<B>)
            return EBaseType::RECORD;
        else
            return EBaseType::UNKNOWN;
    }

    //----------- 2.  Deduce qualifier ----------------------------------------------------
    template <typename T> consteval ETypeQual deduce_qual()
    {
        if constexpr (std::is_pointer_v<T>)
        {
            using Pointee = std::remove_pointer_t<T>;
            constexpr bool ptr_const = std::is_const_v<std::remove_reference_t<T>>;
            constexpr bool obj_const = std::is_const_v<Pointee>;

            if constexpr (ptr_const && obj_const)
                return ETypeQual::CONST_PTR_TO_CONST;
            else if constexpr (ptr_const && !obj_const)
                return ETypeQual::CONST_PTR;
            else if constexpr (!ptr_const && obj_const)
                return ETypeQual::PTR_TO_CONST;
            else
                return ETypeQual::PTR;
        }
        else if constexpr (std::is_lvalue_reference_v<T>)
        {
            using Refee = std::remove_reference_t<T>;
            return std::is_const_v<Refee> ? ETypeQual::L_REF_TO_CONST : ETypeQual::L_REF;
        }
        else if constexpr (std::is_rvalue_reference_v<T>)
        {
            using Refee = std::remove_reference_t<T>;
            return std::is_const_v<Refee> ? ETypeQual::R_REF_TO_CONST : ETypeQual::R_REF;
        }
        else
        {
            return ETypeQual::VALUE;
        }
    }

    template <typename T> consteval QualType make_qual_type()
    {
        return {
            static_cast<std::uint8_t>(p_to_underlying(deduce_base<std::remove_cvref_t<T>>())),
            static_cast<std::uint8_t>(p_to_underlying(deduce_qual<T>()))
        };
    }

    // Because our reflection system currently supports only T*, const T, and const T&,
    // this function is already sufficient for our present needs.
    template <class T> struct TRemoveOneModifier
    {
        using type = std::remove_cv_t<std::remove_reference_t<std::remove_pointer_t<T>>>;
    };

    template <class T> using remove_one_modifier_t = typename TRemoveOneModifier<T>::type;

    /** @brief Produce @ref QualType for a C++ type `T`. */
    template <typename T> constexpr inline QualType __builtin_qual_type = make_qual_type<T>();

    template <typename T> constexpr inline QualType* builtin_qual_type_ptr()
    {
        return &__builtin_qual_type<T>;
    }
} // namespace lux::meta
