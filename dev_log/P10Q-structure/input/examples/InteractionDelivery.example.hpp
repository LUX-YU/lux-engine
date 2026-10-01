#pragma once
#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace lux::editor::workbench::detail
{
    template<class R>
    concept VoidDeliveryResult =
        std::default_initializable<R> &&
        std::move_constructible<R> &&
        requires(R& result, const R& observed)
        {
            typename R::value_type;
            typename R::error_type;
            requires std::same_as<typename R::value_type, void>;
            { static_cast<bool>(observed) } -> std::same_as<bool>;
            { result.error() } -> std::same_as<typename R::error_type&>;
        };

    template<class F, class R>
    concept DeliveryAction =
        std::invocable<F&> &&
        std::same_as<std::invoke_result_t<F&>, R>;

    enum class EInputDeliveryStage : std::uint8_t
    {
        BEGIN, PREVIEW, COMMIT, CANCEL, COMPLETE
    };

    template<class Validate, class Cancel,
             class Begin, class Preview, class Commit>
    requires std::invocable<Validate&> &&
        VoidDeliveryResult<std::invoke_result_t<Validate&>> &&
        DeliveryAction<Cancel, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Begin, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Preview, std::invoke_result_t<Validate&>> &&
        DeliveryAction<Commit, std::invoke_result_t<Validate&>>
    auto deliverInput(
        EInputDeliveryStage& stage,
        bool commit,
        Validate&& validate,
        Cancel&& cancel,
        Begin&& begin,
        Preview&& preview,
        Commit&& finish) -> std::invoke_result_t<Validate&>
    {
        using Result = std::invoke_result_t<Validate&>;
        if (stage != EInputDeliveryStage::CANCEL &&
            stage != EInputDeliveryStage::COMPLETE)
        {
            if (auto result = std::invoke(validate); !result)
            {
                return result;
            }
        }
        if (stage == EInputDeliveryStage::CANCEL)
        {
            if (auto result = std::invoke(cancel); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::BEGIN)
        {
            if (auto result = std::invoke(begin); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::PREVIEW;
        }
        if (stage == EInputDeliveryStage::PREVIEW)
        {
            if (auto result = std::invoke(preview); !result)
            {
                return result;
            }
            stage = commit ? EInputDeliveryStage::COMMIT
                           : EInputDeliveryStage::COMPLETE;
        }
        if (stage == EInputDeliveryStage::COMMIT)
        {
            if (auto result = std::invoke(finish); !result)
            {
                return result;
            }
            stage = EInputDeliveryStage::COMPLETE;
        }
        return Result{};
    }
}
