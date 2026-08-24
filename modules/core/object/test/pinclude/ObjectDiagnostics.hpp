#pragma once

#if !defined(LUX_OBJECT_TEST_DIAGNOSTICS)
#error Object diagnostics are available only to the benchmark test build
#endif

#include <cstdint>

#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/Signal.hpp>
#include <lux/engine/object/detail/MessageEnvelope.hpp>

namespace lux::object::detail
{
    struct ObjectDiagnosticsAccess final
    {
        [[nodiscard]] static std::uint64_t storageGrowthCount(const LuxObject &object) noexcept
        {
            return object.storageGrowthCountForTest();
        }

        [[nodiscard]] static std::size_t ownedConnectionCount(const LuxObject &object) noexcept
        {
            return object.ownedConnectionCountForTest();
        }

        [[nodiscard]] static std::size_t incomingConnectionCount(const LuxObject &object) noexcept
        {
            return object.incomingConnectionCountForTest();
        }

        template <class SignalType>
        [[nodiscard]] static std::size_t denseIndex(const SignalType &signal) noexcept
        {
            return signal.descriptor_.dense_index_;
        }

        template <class SignalType>
        [[nodiscard]] static std::size_t lineageSize(const SignalType &signal) noexcept
        {
            return signal.descriptor_.lineage_size_;
        }

        static void resetMessageStorage() noexcept
        {
            resetMessageStorageForTest();
        }

        [[nodiscard]] static std::size_t inlineMessageStorageCount() noexcept
        {
            return inlineMessageStorageCountForTest();
        }

        [[nodiscard]] static std::size_t heapMessageStorageCount() noexcept
        {
            return heapMessageStorageCountForTest();
        }

        static void closeForTest(LuxObject &object)
        {
            object.closeForTest();
        }
    };
} // namespace lux::object::detail
