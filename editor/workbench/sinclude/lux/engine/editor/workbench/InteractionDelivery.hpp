#pragma once
#include <cstdint>
#include <type_traits>

namespace lux::editor::workbench::detail
{
    enum class EInputDeliveryStage : std::uint8_t
    {
        BEGIN,
        PREVIEW,
        COMMIT,
        CANCEL,
        COMPLETE
    };

    // Synchronous, borrowed actions only. The concrete owner retains its immutable source stamp,
    // typed payload and bounded queue. A failed phase retains both payload and the exact phase.
    template <class Validate, class Cancel, class Begin, class Preview, class Commit>
    auto deliverInput(
        EInputDeliveryStage& stage,
        bool commit,
        Validate&& validate,
        Cancel&& cancel,
        Begin&& begin,
        Preview&& preview,
        Commit&& finish
    ) -> std::invoke_result_t<Validate&>
    {
        using Result = std::invoke_result_t<Validate&>;
        if (stage != EInputDeliveryStage::CANCEL && stage != EInputDeliveryStage::COMPLETE)
            if (auto result = validate(); !result)
                return result;
        if (stage == EInputDeliveryStage::CANCEL)
        {
            if (auto result = cancel(); !result)
                return result;
            stage = EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::BEGIN)
        {
            if (auto result = begin(); !result)
                return result;
            stage = EInputDeliveryStage::PREVIEW;
        }
        if (stage == EInputDeliveryStage::PREVIEW)
        {
            if (auto result = preview(); !result)
                return result;
            stage = commit ? EInputDeliveryStage::COMMIT : EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::COMMIT)
        {
            if (auto result = finish(); !result)
                return result;
            stage = EInputDeliveryStage::COMPLETE;
        }
        return Result{};
    }
}
