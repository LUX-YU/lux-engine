#pragma once
// ============================================================================
//  FeatureOps.hpp — typed-op SSOT.
//
//  Declare a feature's commands ONCE as op-descriptor types; derive the
//  register / unregister / id-lookup / push-routing from the type instead of
//  hand-copying them across four sites (the old XxxOperationIds::fromOps raw
//  index, register_ops_fn, unregister_ops_fn, and each proxy's push call).
//
//  An "op descriptor" states WHAT a command is — its POD payload, its kind, its
//  dispatcher name — not HOW to wire it. Handler bodies stay hand-written (they
//  ARE the feature's semantics); only the mechanical plumbing is generated.
//
//  This header is client-safe (only depends on RenderCommTypes). The server
//  half — binding handlers + generating the factory's register/unregister fns —
//  lives in comm/server/FeatureOpRegistrar.hpp.
//
//  See .internal/feature-classification-and-2d-coupling-2026-06-21.md Part II §9.
// ============================================================================

#include <lux/engine/function/render/client/protocol/RenderCommTypes.hpp>
#include <lux/cxx/compile_time/type_info.hpp> // lux::cxx::type_hash (compile-time type id)

#include <array>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace lux::render
{
    /// What dispatch shape a command has. Selects the push/register pair and the
    /// default comm opcode (a descriptor may override the opcode explicitly via
    /// `static constexpr OpCode opcode` — e.g. a reply-bearing op on CommandOp).
    enum class EOpKind : std::uint8_t
    {
        Stream,   ///< per-frame fire-and-forget POD, no reply          (CommandOp,  push)
        Resource, ///< create / lifecycle carrying a reply              (ResourceOp, pushResource)
        Bulk,     ///< N copies of one POD in a single command          (BulkData,   pushBulk)
        Blob,     ///< payload embeds a BlobRef to variable-length data (CommandOp,  pushBlob + push)
        Param,    ///< Stream specialised as a reflected param setter   (CommandOp,  push)
    };

    /// Which transport admits an operation. This is deliberately independent
    /// of EOpKind: kind describes the wire payload shape, while lane describes
    /// lifetime and scheduling semantics.
    enum class EOperationLane : std::uint8_t
    {
        Program, ///< retained-state updates and lexical frame programs
        Control, ///< scene/view/feature/query/destroy; legal with no frame open
        Upload,  ///< owning persistent payload; bounded and non-blocking
    };

    /// An op descriptor: a tag type carrying { Payload, kind, name }. The server
    /// additionally binds a handler (see ServerOp) — kept separate so the same
    /// descriptor is usable client-side, where no handler exists.
    template <class Op>
    concept FeatureOpDesc = requires {
        typename Op::Payload;
        requires std::is_trivially_copyable_v<typename Op::Payload>;
        { Op::kind } -> std::convertible_to<EOpKind>;
        { Op::lane } -> std::convertible_to<EOperationLane>;
        { Op::name } -> std::convertible_to<const char*>;
    };

    namespace detail
    {
        constexpr OpCode defaultOpcode(EOpKind kind) noexcept
        {
            switch (kind)
            {
            case EOpKind::Resource:
                return opcodes::ResourceOp;
            case EOpKind::Bulk:
                return opcodes::BulkData;
            default:
                return opcodes::CommandOp; // Stream / Blob / Param
            }
        }

        template <class Op, class = void> struct OpcodeOf : std::integral_constant<OpCode, defaultOpcode(Op::kind)>
        {
        };

        template <class Op>
        struct OpcodeOf<Op, std::void_t<decltype(Op::opcode)>> : std::integral_constant<OpCode, Op::opcode>
        {
        };
    } // namespace detail

    /// Comm opcode an op routes on: its kind's default, or the op's override.
    template <class Op> inline constexpr OpCode opcode_of_v = detail::OpcodeOf<Op>::value;

    // ── reply_type_id derived from the Reply TYPE (kills hand-picked ids) ──────
    //  Same Reply shape → same id on both ends (constexpr, header-shared); no central
    //  table, no cross-feature collision. Fill CommandTraits<P>::reply_type_id with this
    //  when migrating a reply-bearing op.
    //
    //  Reuses lux-cxx's compile-time type hash (lux::cxx::type_hash) instead of a
    //  hand-rolled __FUNCSIG__ + FNV-1a. It is a per-build hash of the stripped type
    //  name; we truncate the 64-bit value to TypeId (uint32_t). This is a WIRE-ONLY shape
    //  guard — client and server share one build so the values agree — not a persisted or
    //  cross-build identifier (see type_info.hpp's caveat).
    template <class Reply>
    inline constexpr TypeId reply_type_id_of_v = static_cast<TypeId>(lux::cxx::type_hash<Reply>());

    // ── Client-side typed op-id store — replaces hand-written XxxOperationIds ──
    //  Built from the register reply's ops[]; looked up by op TYPE, never by a
    //  raw index. Ops... order must match the server registrar's order (both come
    //  from the same per-feature declaration).
    template <FeatureOpDesc... Ops> class FeatureOpIds
    {
    public:
        static constexpr std::size_t kCount = sizeof...(Ops);

        FeatureOpIds() = default;

        [[nodiscard]] static FeatureOpIds fromOps(const TypeId* ops, std::uint32_t count) noexcept
        {
            FeatureOpIds out;
            const std::size_t n = count < kCount ? count : kCount;
            for (std::size_t i = 0; i < n; ++i)
                out.ids_[i] = ops[i];
            return out;
        }

        /// The dynamic TypeId of op `Op` (compile-time index into the list).
        template <class Op> [[nodiscard]] TypeId id() const noexcept
        {
            constexpr std::size_t i = indexOf<Op>();
            static_assert(i < kCount, "op is not part of this feature's declared op list");
            return ids_[i];
        }

        /// True once the feature is registered (first op resolved).
        [[nodiscard]] bool valid() const noexcept
        {
            return kCount != 0 && ids_[0] != kInvalidTypeId;
        }

    private:
        template <class Op> static constexpr std::size_t indexOf() noexcept
        {
            std::size_t i = 0, result = kCount;
            (void)((std::is_same_v<Op, Ops> ? (result = i, true) : (++i, false)) || ...);
            return result;
        }

        static constexpr std::size_t kStore = kCount == 0 ? 1 : kCount;

        static constexpr std::array<TypeId, kStore> allInvalid() noexcept
        {
            std::array<TypeId, kStore> a{};
            for (TypeId& v : a)
                v = kInvalidTypeId;
            return a;
        }

        std::array<TypeId, kStore> ids_ = allInvalid();
    };

} // namespace lux::render
