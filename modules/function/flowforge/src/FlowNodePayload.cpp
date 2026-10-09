#include <lux/engine/flowforge/FlowNodePayload.hpp>

namespace lux::flowforge
{
    FlowNodePayload::FlowNodePayload(
        object::CodeLease code,
        cxx::TypeToken type,
        void* value,
        Destroy destroy,
        Clone clone
    ) noexcept
        : code_(std::move(code)), type_(type), value_(value), destroy_(destroy), clone_(clone)
    {
    }

    FlowNodePayload::~FlowNodePayload()
    {
        reset();
    }

    FlowNodePayload::FlowNodePayload(FlowNodePayload&& other) noexcept
        : code_(std::move(other.code_)), type_(std::exchange(other.type_, {})),
          value_(std::exchange(other.value_, nullptr)), destroy_(std::exchange(other.destroy_, nullptr)),
          clone_(std::exchange(other.clone_, nullptr))
    {
    }

    FlowNodePayload& FlowNodePayload::operator=(FlowNodePayload&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            code_ = std::move(other.code_);
            type_ = std::exchange(other.type_, {});
            value_ = std::exchange(other.value_, nullptr);
            destroy_ = std::exchange(other.destroy_, nullptr);
            clone_ = std::exchange(other.clone_, nullptr);
        }
        return *this;
    }

    void FlowNodePayload::reset() noexcept
    {
        // The module retains the lease through the plugin destructor and its return.
        auto code = std::move(code_);
        auto* value = std::exchange(value_, nullptr);
        const auto destroy = std::exchange(destroy_, nullptr);
        type_ = {};
        clone_ = nullptr;
        if (value)
        {
            destroy(value);
        }
    }

    FlowForgeResult<FlowNodePayload> FlowNodePayload::clone() const noexcept
    {
        if (!value_)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "cannot clone an empty node payload"}
            );
        }
        auto code = code_;
        const auto type = type_;
        const auto destroy = destroy_;
        const auto clone = clone_;
        auto result = clone(value_);
        if (!result)
        {
            return cxx::unexpected(std::move(result.error()));
        }
        if (!*result)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "node clone returned no payload"}
            );
        }
        return FlowNodePayload{std::move(code), type, *result, destroy, clone};
    }
} // namespace lux::flowforge
