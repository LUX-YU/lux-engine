#pragma once

#include <lux/engine/simulation/ecs/core/detail/EcsChangeJournalAccess.hpp>

namespace lux::simulation::ecs::detail
{
    /** Non-installed instrumentation and fault-injection seam. */
    struct EcsChangeJournalTestAccess final
    {
        [[nodiscard]] static std::uint64_t recordWriteCount(
            const EcsChangeJournal& journal
        ) noexcept
        {
            return EcsChangeJournalAccess::log(journal)
                .recordWriteCountForTest();
        }

        [[nodiscard]] static std::size_t dynamicBlockAcquisitions(
            const EcsChangeJournal& journal
        ) noexcept
        {
            return EcsChangeJournalAccess::log(journal)
                .dynamicBlockAcquisitionsForTest();
        }

        [[nodiscard]] static std::uint64_t streamBindCount(
            const EcsChangeJournal& journal
        ) noexcept
        {
            return EcsChangeJournalAccess::log(journal)
                .streamBindCountForTest();
        }

        [[nodiscard]] static std::uint64_t perRecordLookupCount(
            const EcsChangeJournal& journal
        ) noexcept
        {
            return EcsChangeJournalAccess::log(journal)
                .perRecordLookupCountForTest();
        }

        [[nodiscard]] static std::size_t activeBlockCount(
            const EcsChangeJournal& journal
        ) noexcept
        {
            return EcsChangeJournalAccess::log(journal)
                .activeBlockCountForTest();
        }

        static void failNextStreamDescriptor(EcsChangeJournal& journal) noexcept
        {
            EcsChangeJournalAccess::log(journal)
                .failNextStreamDescriptorForTest();
        }

        static void failNextBlockAcquisition(EcsChangeJournal& journal) noexcept
        {
            EcsChangeJournalAccess::log(journal)
                .failNextBlockAcquisitionForTest();
        }

        static void failNextBlockAttach(EcsChangeJournal& journal) noexcept
        {
            EcsChangeJournalAccess::log(journal)
                .failNextBlockAttachForTest();
        }
    };
} // namespace lux::simulation::ecs::detail
