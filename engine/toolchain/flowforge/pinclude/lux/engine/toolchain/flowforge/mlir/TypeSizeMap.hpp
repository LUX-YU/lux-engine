#pragma once

#include "FlowForgeVersionCompat.h"
#include FLOWFORGE_ARITH_DIALECT_INCLUDE

#include <lux/engine/meta/RuntimeObject.hpp>

#include <cstddef>
#include <cstdint>

namespace lux::flowforge
{
    template<std::size_t Bits>
    struct TypeSizeMap;

    template<>
    struct TypeSizeMap<8>
    {
        using type = std::uint8_t;

        static auto getLLVMType(mlir::OpBuilder& builder)
        {
            return builder.getI8Type();
        }

        static auto getAttr(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.getIntegerAttr(
                builder.getI8Type(),
                *static_cast<const std::uint8_t*>(object.data()));
        }

        static auto getIndex(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.create<mlir::arith::ConstantIndexOp>(
                builder.getUnknownLoc(),
                *static_cast<const std::uint8_t*>(object.data()));
        }
    };

    template<>
    struct TypeSizeMap<16>
    {
        using type = std::uint16_t;

        static auto getLLVMType(mlir::OpBuilder& builder)
        {
            return FLOWFORGE_GET_I16_TYPE(builder);
        }

        static auto getAttr(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.getIntegerAttr(
                FLOWFORGE_GET_I16_TYPE(builder),
                *static_cast<const std::uint16_t*>(object.data()));
        }

        static auto getIndex(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.create<mlir::arith::ConstantIndexOp>(
                builder.getUnknownLoc(),
                *static_cast<const std::uint16_t*>(object.data()));
        }
    };

    template<>
    struct TypeSizeMap<32>
    {
        using type = std::uint32_t;

        static auto getLLVMType(mlir::OpBuilder& builder)
        {
            return builder.getI32Type();
        }

        static auto getAttr(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.getIntegerAttr(
                builder.getI32Type(),
                *static_cast<const std::uint32_t*>(object.data()));
        }

        static auto getIndex(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.create<mlir::arith::ConstantIndexOp>(
                builder.getUnknownLoc(),
                *static_cast<const std::uint32_t*>(object.data()));
        }
    };

    template<>
    struct TypeSizeMap<64>
    {
        using type = std::uint64_t;

        static auto getLLVMType(mlir::OpBuilder& builder)
        {
            return builder.getI64Type();
        }

        static auto getAttr(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.getIntegerAttr(
                builder.getI64Type(),
                *static_cast<const std::uint64_t*>(object.data()));
        }

        static auto getIndex(
            mlir::OpBuilder& builder,
            const lux::meta::RuntimeObject& object)
        {
            return builder.create<mlir::arith::ConstantIndexOp>(
                builder.getUnknownLoc(),
                *static_cast<const std::uint64_t*>(object.data()));
        }
    };
}
