#pragma once
#include "Meta.hpp"
#include <algorithm>
#include <array>

namespace lux::meta
{
    //--------------------------------------------------------------------------------------------------
    // 1.  Qualifier compatibility tables (9×9 for ETypeQual)
    //--------------------------------------------------------------------------------------------------

    /**
     * @brief 9×9 table describing *initialisation* compatibility.
     *
     * Row = destination qualifier, column = source qualifier.
     * Value 1 means “allowed”.
     */
    static inline constexpr bool gInit[9 /*dst*/][9 /*src*/] = {
        // Dst\Src:    Val   LRef  RRef  CRef  RRefC Ptr   PtrC  CPtr  CPtrC
        /* Value     */ { 1,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* LRef      */ { 1,    1,    0,    0,    0,    0,    0,    0,    0 },
        /* RRef      */ { 0,    0,    1,    1,    0,    0,    0,    0,    0 },
        /* CRef      */ { 1,    0,    0,    1,    0,    0,    0,    0,    0 },
        /* RRefC     */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* Ptr       */ { 0,    0,    0,    0,    0,    1,    0,    0,    0 },
        /* PtrC      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* CPtr      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* CPtrC     */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 }
    };

    /**
     * @brief 9×9 table describing *assignment* compatibility.
     *
     * Row = destination qualifier, column = source qualifier.
     * Value 1 means “allowed”.
     *
     * Note: ConstRef (CRef) is already rejected at the outer layer, so it
     * never reaches assignment.
     */
    static inline constexpr bool gAssign[9 /*dst*/][9 /*src*/] = {
        // Dst\Src:    Val   LRef  RRef  CRef  RRefC   Ptr PtrC  CPtr  CPtrC
        /* Value     */ { 1,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* LRef      */ { 1,    1,    0,    0,    0,    0,    0,    0,    0 },
        /* RRef      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* CRef      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* RRefC     */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* Ptr       */ { 0,    0,    0,    0,    0,    1,    0,    0,    0 },
        /* PtrC      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* CPtr      */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 },
        /* CPtrC     */ { 0,    0,    0,    0,    0,    0,    0,    0,    0 }
    };

    //--------------------------------------------------------------------------------------------------
    // 2.  Helper – fast check when base types match
    //--------------------------------------------------------------------------------------------------
    static inline bool qualCompatible(const QualType& dst, const QualType& src, bool for_init) noexcept
    {
        return for_init ? gInit[dst.qual][src.qual] : gAssign[dst.qual][src.qual];
    }

    static constexpr inline std::array<std::array<bool, p_base_type_num>, p_base_type_num> p_lossless_convert_table = {{
        //            src:    Void    Bool    Int8   Uint8   Int16   Uint16  Int32    Uint32   Int64    Uint64   Float    Double  Record  Unknown
        /* dst=Void    */ {{ false,  false,  false,  false,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Bool    */ {{ false,   true,  false,  false,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Int8    */ {{ false,   true,   true,  false,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Uint8   */ {{ false,   true,  false,   true,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Int16   */ {{ false,   true,   true,   true,   true,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Uint16  */ {{ false,   true,  false,   true,  false,   true,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Int32   */ {{ false,   true,   true,   true,   true,   true,   true,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Uint32  */ {{ false,   true,  false,   true,  false,   true,  false,    true,   false,   false,   false,   false,   false,   false}},
        /* dst=Int64   */ {{ false,   true,   true,   true,   true,   true,   true,    true,    true,   false,   false,   false,   false,   false}},
        /* dst=Uint64  */ {{ false,   true,  false,   true,  false,   true,  false,    true,   false,    true,   false,   false,   false,   false}},
        /* dst=Float   */ {{ false,   true,   true,   true,   true,   true,  false,   false,   false,   false,    true,   false,   false,   false}},
        /* dst=Double  */ {{ false,   true,   true,   true,   true,   true,   true,    true,   false,   false,    true,   true,    false,    false}},
        /* dst=Record  */ {{ false,  false,  false,  false,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
        /* dst=Unknown */ {{ false,  false,  false,  false,  false,  false,  false,   false,   false,   false,   false,   false,   false,   false}},
    }};

    //--------------------------------------------------------------------------------------------------
    // 2.1  Helper - support for lossless implicit numeric type conversions
    //--------------------------------------------------------------------------------------------------
    static constexpr inline bool canImplicitlyConvert(const QualType& dst, const QualType& src) noexcept
    {
        if (dst.qual != p_to_underlying(ETypeQual::Value) ||
            src.qual != p_to_underlying(ETypeQual::Value))
            return false;
        
        if (dst.base >= p_base_type_num || src.base >= p_base_type_num)
            return false;

        return p_lossless_convert_table[dst.base][src.base];
    }

    //--------------------------------------------------------------------------------------------------
    // 3.  Helper – Record type compatibility check
    //--------------------------------------------------------------------------------------------------
    static inline bool recordCompatible(const RefType* dst, const RefType* src) noexcept
    {
        // Identical record types are always compatible — and this must not
        // depend on RefType.ptr, which is null for constexpr ref_type_of_v
        // instances (e.g. reflected function parameter types).
        if (dst->hash == src->hash)
            return true;

        auto *dst_class = static_cast<const RefClass*>(dst->ptr);
        auto *src_class = static_cast<const RefClass*>(src->ptr);
        if(!dst_class || !src_class)
            return false;

        if (dst_class == src_class)
            return true;

        auto iter = std::find(
            src_class->parent_chain.begin(), 
            src_class->parent_chain.end(), 
            dst_class->hash
        );

        if (iter != src_class->parent_chain.end())
            return true;

        return false;
    }

    static inline bool p_basic_type_compatible(const RefType* dst, const RefType* src) noexcept
    {
        if(p_is_base_fundamental(dst->qtype) && p_is_base_fundamental(src->qtype))
        {
            return canImplicitlyConvert(dst->qtype, src->qtype);
        }

        if(p_is_base_record(dst->qtype) && p_is_base_record(src->qtype))
        {
            return recordCompatible(dst, src);
        }

        return false;
    }

    //--------------------------------------------------------------------------------------------------
    // 4.  Public interface
    //--------------------------------------------------------------------------------------------------
    static inline bool canInitialize(const RefType* dst, const RefType* src) noexcept
    {
        if(!p_basic_type_compatible(dst, src))
        {
            return false;
        }

        return qualCompatible(dst->qtype, src->qtype, true);
    }

    static inline bool canAssign(const RefType* dst, const RefType* src) noexcept
    {
        if(!p_basic_type_compatible(dst, src))
        {
            return false;
        }

        return qualCompatible(dst->qtype, src->qtype, false);
    }
} // namespace lux::meta
