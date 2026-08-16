#pragma once
// ============================================================================
//  FeatureOpRegistrar.hpp — server half of typed-op.
//
//  Bind each op descriptor (FeatureOps.hpp) to its server-only handler and
//  generate the FeatureFactory's register_ops_fn / unregister_ops_fn from the
//  ordered (op, handler) list — no hand-written allocateAndRegister*/freeSlot
//  loops, no opcode/kind mismatch possible.
//
//  Include from a feature's *OperationHandlers.cpp (needs the server Dispatcher).
//
//      using FooOps = FeatureOpRegistrar< ServerOp<FooSetOp,    &handleFooSet>,
//                                         ServerOp<FooBulkOp,   &handleFooBulk> >;
//      const FeatureFactory kFooFactory{
//          &fooCreate, &FooOps::registerAll, &FooOps::unregisterAll, "Foo" };
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/protocol/FeatureOps.hpp>
#include <lux/engine/function/render/client/protocol/FeatureParamsOperation.hpp> // registerFeatureParamsOp (EOpKind::Param)

#include <array>
#include <cstddef>
#include <cstdint>

namespace lux::render
{
    /// Binds an op descriptor to its handler. The handler signature is fixed by
    /// the op's kind: void(Ctx&, const Payload&) for unary ops (Stream/Resource/
    /// Blob), void(Ctx&, std::span<const Payload>) for Bulk. A Param-kind op uses
    /// the SHARED setParams handler (registerFeatureParamsOp), so its Handler is
    /// omitted — default nullptr.
    template <FeatureOpDesc Op, auto Handler = nullptr>
    struct ServerOp
    {
        using Desc = Op;
        static constexpr auto handler = Handler;
    };

    /// Generates the factory's register/unregister fns from an ordered op list.
    /// The ops[] order it emits matches FeatureOpIds<Ops...> on the client (both
    /// derive from the same per-feature declaration order).
    template <class... ServerOps>
    struct FeatureOpRegistrar
    {
        using Dispatcher = GeneralRenderServer::Dispatcher;

        /// Index into the emitted ops[] of the (first) Param-kind op, or -1 when the
        /// feature declares none. Feed straight into FeatureFactory.param_set_op_index so
        /// the settings panel finds the setParams op without a hand-counted magic number.
        static constexpr int kParamSetOpIndex = []() constexpr -> int {
            if constexpr (sizeof...(ServerOps) == 0)
                return -1;
            else
            {
                const std::array<EOpKind, sizeof...(ServerOps)> kinds{ ServerOps::Desc::kind... };
                for (std::size_t i = 0; i < kinds.size(); ++i)
                    if (kinds[i] == EOpKind::Param) return static_cast<int>(i);
                return -1;
            }
        }();

        static std::uint32_t registerAll(void* dispatcher, TypeId* out_ops, std::uint32_t /*max_ops*/)
        {
            auto& d = *static_cast<Dispatcher*>(dispatcher);
            std::uint32_t i = 0;
            ((out_ops[i++] = registerOne<ServerOps>(d)), ...);
            return sizeof...(ServerOps);
        }

        static void unregisterAll(void* dispatcher, const TypeId* ops, std::uint32_t op_count)
        {
            auto& d = *static_cast<Dispatcher*>(dispatcher);
            constexpr std::array<OpCode, sizeof...(ServerOps)> kOpcodes = {
                opcode_of_v<typename ServerOps::Desc>...
            };
            for (std::uint32_t i = 0; i < op_count && i < kOpcodes.size(); ++i)
                d.freeSlot(kOpcodes[i], ops[i]);
        }

    private:
        template <class SOp>
        static TypeId registerOne(Dispatcher& d)
        {
            using Op = typename SOp::Desc;
            constexpr OpCode oc = opcode_of_v<Op>;
            if constexpr (Op::kind == EOpKind::Param)
                return registerFeatureParamsOp(&d);   // shared SetFeatureParams handler (no per-op handler)
            else if constexpr (Op::kind == EOpKind::Bulk)
                return d.allocateAndRegisterBulk<typename Op::Payload, SOp::handler>(oc, Op::name);
            else
                return d.allocateAndRegisterUnary<typename Op::Payload, SOp::handler>(oc, Op::name);
        }
    };

} // namespace lux::render
