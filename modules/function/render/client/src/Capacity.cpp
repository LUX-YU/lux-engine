#include <lux/engine/function/render/Capacity.hpp>

#include <algorithm>
#include <limits>
#include <string_view>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] CapacityDomainId own(CapacityDomainIdView id)
        {
            return CapacityDomainId{id.name()};
        }

        [[nodiscard]] bool isValidCapacityDomainNameImpl(
            std::string_view name) noexcept
        {
            if (name.empty() || name.front() == '.' || name.back() == '.')
                return false;

            bool has_dot = false;
            bool previous_dot = false;
            for (const char value : name)
            {
                const bool dot = value == '.';
                if (dot)
                {
                    if (previous_dot)
                        return false;
                    has_dot = true;
                }
                else if (!((value >= 'a' && value <= 'z') ||
                           (value >= '0' && value <= '9') ||
                           value == '_' || value == '-'))
                {
                    return false;
                }
                previous_dot = dot;
            }
            return has_dot;
        }

        [[nodiscard]] bool isCapacityDomainCollision(
            CapacityDomainIdView left,
            CapacityDomainIdView right) noexcept
        {
            return left.hash() == right.hash() && left.name() != right.name();
        }

        [[nodiscard]] constexpr std::uint64_t saturatedAdd(
            std::uint64_t left,
            std::uint64_t right) noexcept
        {
            return left > std::numeric_limits<std::uint64_t>::max() - right
                ? std::numeric_limits<std::uint64_t>::max()
                : left + right;
        }

        [[nodiscard]] constexpr std::uint64_t saturatedMultiply(
            std::uint64_t left,
            std::uint64_t right) noexcept
        {
            return right != 0u &&
                    left > std::numeric_limits<std::uint64_t>::max() / right
                ? std::numeric_limits<std::uint64_t>::max()
                : left * right;
        }

        [[nodiscard]] constexpr std::uint64_t availableBudget(
            const CapacityDeviceFacts& device) noexcept
        {
            if (device.vram_budget_bytes == 0u)
                return std::numeric_limits<std::uint64_t>::max();
            const auto free = device.vram_budget_bytes > device.vram_usage_bytes
                ? device.vram_budget_bytes - device.vram_usage_bytes
                : 0u;
            return free - free / 10u;
        }

        [[nodiscard]] constexpr std::uint64_t estimateBytes(
            std::uint64_t units,
            const CapacityDomainDescriptor& descriptor) noexcept
        {
            if (units == 0u || descriptor.bytes_per_granule == 0u)
                return 0u;
            const auto granules = saturatedAdd(
                units,
                descriptor.units_per_granule - 1u) /
                descriptor.units_per_granule;
            return saturatedMultiply(
                granules,
                descriptor.bytes_per_granule);
        }

        [[nodiscard]] CapacityShortfall failure(
            CapacityDomainIdView domain,
            CapacityPlanError error,
            CapacityPlanReason reason,
            std::uint64_t requested,
            std::uint64_t effective,
            std::uint64_t bytes,
            std::uint64_t available)
        {
            return CapacityShortfall{
                own(domain),
                error,
                reason,
                requested,
                effective,
                bytes,
                available};
        }
    } // namespace

    bool isValidCapacityDomainName(std::string_view name) noexcept
    {
        return isValidCapacityDomainNameImpl(name);
    }

    void CapacityRequest::set(
        CapacityDomainIdView domain,
        CapacityValue value)
    {
        for (auto& entry : domains)
        {
            if (entry.domain.view() == domain)
            {
                entry.value = value;
                return;
            }
        }
        domains.push_back(CapacityRequestEntry{own(domain), value});
    }

    const CapacityValue* CapacityRequest::find(
        CapacityDomainIdView domain) const noexcept
    {
        for (const auto& entry : domains)
            if (entry.domain.view() == domain)
                return &entry.value;
        return nullptr;
    }

    lux::cxx::expected<void, CapacityCatalogError>
    CapacityCatalog::add(CapacityDomainDescriptor descriptor)
    {
        if (!descriptor.id.isValid() ||
            !isValidCapacityDomainName(descriptor.id.name()) ||
            descriptor.device_limit == 0u ||
            descriptor.protocol_limit == 0u ||
            descriptor.automatic_target == 0u ||
            descriptor.minimum == 0u ||
            descriptor.units_per_granule == 0u ||
            descriptor.bytes_per_granule == 0u ||
            descriptor.minimum > descriptor.device_limit ||
            descriptor.minimum > descriptor.protocol_limit)
        {
            return lux::cxx::unexpected(
                CapacityCatalogError::INVALID_DESCRIPTOR);
        }
        for (const auto& current : descriptors_)
        {
            if (current.id.view() == descriptor.id.view())
            {
                return lux::cxx::unexpected(
                    CapacityCatalogError::DUPLICATE_DOMAIN);
            }
            if (isCapacityDomainCollision(
                    current.id.view(),
                    descriptor.id.view()))
            {
                return lux::cxx::unexpected(
                    CapacityCatalogError::ID_COLLISION);
            }
        }
        descriptors_.push_back(std::move(descriptor));
        return {};
    }

    const CapacityDomainDescriptor* CapacityCatalog::find(
        CapacityDomainIdView id) const noexcept
    {
        for (const auto& descriptor : descriptors_)
            if (descriptor.id.view() == id)
                return &descriptor;
        return nullptr;
    }

    const CapacityDomainPlan* CapacityPlan::find(
        CapacityDomainIdView domain) const noexcept
    {
        for (const auto& entry : domains)
            if (entry.domain.view() == domain)
                return &entry;
        return nullptr;
    }

    std::uint64_t CapacityPlan::effective(
        CapacityDomainIdView domain) const noexcept
    {
        const auto* entry = find(domain);
        return entry ? entry->effective : 0u;
    }

    lux::cxx::expected<CapacityPlan, CapacityShortfall>
    makeCapacityPlan(
        const CapacityRequest& request,
        const CapacityDeviceFacts& device,
        const CapacityCatalog& catalog)
    {
        const auto available = availableBudget(device);

        for (std::size_t index = 0u; index < request.domains.size(); ++index)
        {
            const auto& entry = request.domains[index];
            if (!entry.domain.isValid() ||
                !isValidCapacityDomainName(entry.domain.name()) ||
                (entry.value.mode == CapacityRequestMode::EXPLICIT &&
                 entry.value.value == 0u))
            {
                return lux::cxx::unexpected(failure(
                    entry.domain.view(),
                    CapacityPlanError::INVALID_REQUEST,
                    CapacityPlanReason::PROTOCOL_CLAMP,
                    entry.value.value,
                    0u,
                    0u,
                    available));
            }
            for (std::size_t other = index + 1u;
                 other < request.domains.size();
                 ++other)
            {
                if (entry.domain.view() == request.domains[other].domain.view())
                {
                    return lux::cxx::unexpected(failure(
                        entry.domain.view(),
                        CapacityPlanError::DUPLICATE_REQUEST,
                        CapacityPlanReason::PROTOCOL_CLAMP,
                        entry.value.value,
                        0u,
                        0u,
                        available));
                }
            }
            if (!catalog.find(entry.domain.view()))
            {
                return lux::cxx::unexpected(failure(
                    entry.domain.view(),
                    CapacityPlanError::UNKNOWN_DOMAIN,
                    CapacityPlanReason::PROTOCOL_CLAMP,
                    entry.value.value,
                    0u,
                    0u,
                    available));
            }
        }

        std::vector<const CapacityDomainDescriptor*> ordered;
        ordered.reserve(catalog.all().size());
        for (const auto& descriptor : catalog.all())
            ordered.push_back(&descriptor);
        std::ranges::sort(
            ordered,
            {},
            [](const CapacityDomainDescriptor* descriptor)
            {
                return descriptor->id.name();
            });

        CapacityPlan result{};
        result.device = device;
        result.domains.reserve(ordered.size());
        std::uint64_t explicit_bytes = 0u;
        std::uint64_t automatic_bytes = 0u;

        for (const auto* descriptor : ordered)
        {
            const auto* requested = request.find(descriptor->id.view());
            const auto value = requested
                ? *requested
                : CapacityValue::automatic();
            CapacityDomainPlan plan{};
            plan.domain = CapacityDomainId{descriptor->id.name()};
            plan.requested = value.mode == CapacityRequestMode::EXPLICIT
                ? value.value
                : 0u;
            plan.device_limit = descriptor->device_limit;
            plan.protocol_limit = descriptor->protocol_limit;

            if (value.mode == CapacityRequestMode::EXPLICIT)
            {
                if (value.value > descriptor->protocol_limit)
                {
                    return lux::cxx::unexpected(failure(
                        descriptor->id.view(),
                        CapacityPlanError::PROTOCOL_LIMIT,
                        CapacityPlanReason::PROTOCOL_CLAMP,
                        value.value,
                        descriptor->protocol_limit,
                        estimateBytes(value.value, *descriptor),
                        available));
                }
                if (value.value > descriptor->device_limit)
                {
                    return lux::cxx::unexpected(failure(
                        descriptor->id.view(),
                        CapacityPlanError::DEVICE_LIMIT,
                        CapacityPlanReason::DEVICE_CLAMP,
                        value.value,
                        descriptor->device_limit,
                        estimateBytes(value.value, *descriptor),
                        available));
                }
                plan.effective = value.value;
                plan.reason = CapacityPlanReason::REQUESTED;
            }
            else
            {
                plan.effective = std::min({
                    descriptor->automatic_target,
                    descriptor->device_limit,
                    descriptor->protocol_limit});
                if (plan.effective < descriptor->minimum)
                {
                    const bool protocol_won = descriptor->protocol_limit <
                        descriptor->device_limit;
                    return lux::cxx::unexpected(failure(
                        descriptor->id.view(),
                        protocol_won
                            ? CapacityPlanError::PROTOCOL_LIMIT
                            : CapacityPlanError::DEVICE_LIMIT,
                        protocol_won
                            ? CapacityPlanReason::PROTOCOL_CLAMP
                            : CapacityPlanReason::DEVICE_CLAMP,
                        descriptor->automatic_target,
                        plan.effective,
                        estimateBytes(descriptor->minimum, *descriptor),
                        available));
                }
                plan.reason = plan.effective < descriptor->automatic_target
                    ? (descriptor->device_limit < descriptor->protocol_limit
                        ? CapacityPlanReason::DEVICE_CLAMP
                        : CapacityPlanReason::PROTOCOL_CLAMP)
                    : CapacityPlanReason::AUTO_DEFAULT;
            }
            plan.estimated_bytes = estimateBytes(
                plan.effective,
                *descriptor);
            if (value.mode == CapacityRequestMode::EXPLICIT)
                explicit_bytes = saturatedAdd(
                    explicit_bytes,
                    plan.estimated_bytes);
            else
                automatic_bytes = saturatedAdd(
                    automatic_bytes,
                    plan.estimated_bytes);
            result.domains.push_back(std::move(plan));
        }

        if (explicit_bytes > available)
        {
            const auto found = std::ranges::find_if(
                result.domains,
                [&](const auto& plan)
                {
                    const auto* value = request.find(plan.domain.view());
                    return value &&
                        value->mode == CapacityRequestMode::EXPLICIT;
                });
            const auto& plan = found != result.domains.end()
                ? *found
                : result.domains.front();
            return lux::cxx::unexpected(failure(
                plan.domain.view(),
                CapacityPlanError::BUDGET_LIMIT,
                CapacityPlanReason::BUDGET_REJECT,
                plan.requested,
                0u,
                explicit_bytes,
                available));
        }

        const auto automatic_available = available - explicit_bytes;
        if (automatic_bytes <= automatic_available)
            return result;

        std::uint64_t minimum_bytes = 0u;
        for (std::size_t index = 0u; index < result.domains.size(); ++index)
        {
            const auto* value = request.find(result.domains[index].domain.view());
            if (value && value->mode == CapacityRequestMode::EXPLICIT)
                continue;
            minimum_bytes = saturatedAdd(
                minimum_bytes,
                estimateBytes(ordered[index]->minimum, *ordered[index]));
        }
        if (minimum_bytes > automatic_available)
        {
            const auto index = std::ranges::find_if(
                result.domains,
                [&](const auto& plan)
                {
                    const auto* value = request.find(plan.domain.view());
                    return !value ||
                        value->mode == CapacityRequestMode::AUTO;
                }) - result.domains.begin();
            return lux::cxx::unexpected(failure(
                result.domains[index].domain.view(),
                CapacityPlanError::BUDGET_LIMIT,
                CapacityPlanReason::BUDGET_REJECT,
                0u,
                0u,
                minimum_bytes,
                automatic_available));
        }

        const long double ratio = automatic_bytes == minimum_bytes
            ? 0.0L
            : static_cast<long double>(
                  automatic_available - minimum_bytes) /
                static_cast<long double>(automatic_bytes - minimum_bytes);
        automatic_bytes = 0u;
        for (std::size_t index = 0u; index < result.domains.size(); ++index)
        {
            auto& plan = result.domains[index];
            const auto* value = request.find(plan.domain.view());
            if (value && value->mode == CapacityRequestMode::EXPLICIT)
                continue;
            const auto minimum = ordered[index]->minimum;
            plan.effective = minimum + static_cast<std::uint64_t>(
                static_cast<long double>(plan.effective - minimum) * ratio);
            plan.estimated_bytes = estimateBytes(
                plan.effective,
                *ordered[index]);
            plan.reason = CapacityPlanReason::BUDGET_REJECT;
            automatic_bytes = saturatedAdd(
                automatic_bytes,
                plan.estimated_bytes);
        }

        while (automatic_bytes > automatic_available)
        {
            std::size_t selected = result.domains.size();
            std::uint64_t selected_saving = 0u;
            std::uint64_t selected_effective = 0u;
            for (std::size_t index = 0u;
                 index < result.domains.size();
                 ++index)
            {
                auto& plan = result.domains[index];
                const auto* value = request.find(plan.domain.view());
                if ((value && value->mode == CapacityRequestMode::EXPLICIT) ||
                    plan.effective <= ordered[index]->minimum)
                    continue;
                const auto next = std::max(
                    ordered[index]->minimum,
                    ((plan.effective - 1u) /
                        ordered[index]->units_per_granule) *
                        ordered[index]->units_per_granule);
                const auto next_bytes = estimateBytes(next, *ordered[index]);
                const auto saving = plan.estimated_bytes - next_bytes;
                if (saving > selected_saving)
                {
                    selected = index;
                    selected_saving = saving;
                    selected_effective = next;
                }
            }
            if (selected == result.domains.size())
                break;
            auto& plan = result.domains[selected];
            plan.effective = selected_effective;
            plan.estimated_bytes -= selected_saving;
            automatic_bytes -= selected_saving;
        }

        if (automatic_bytes > automatic_available)
        {
            return lux::cxx::unexpected(failure(
                result.domains.front().domain.view(),
                CapacityPlanError::BUDGET_LIMIT,
                CapacityPlanReason::BUDGET_REJECT,
                0u,
                0u,
                automatic_bytes,
                automatic_available));
        }
        return result;
    }
} // namespace lux::render
