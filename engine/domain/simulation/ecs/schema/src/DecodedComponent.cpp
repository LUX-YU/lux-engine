#include <lux/engine/simulation/ecs/DecodedComponent.hpp>

#include <cassert>

namespace lux::simulation::ecs
{
    DecodedComponent::DecodedComponent(lux::cxx::TypeToken type, std::size_t bytes, void *value, Destroy destroy,
                                       Install install, std::shared_ptr<const void> code) noexcept
        : type_(type), bytes_(bytes), value_(value), destroy_(destroy), install_(install), code_(std::move(code))
    {
    }

    DecodedComponent::DecodedComponent(DecodedComponent &&other) noexcept
        : type_(other.type_), bytes_(other.bytes_), value_(std::exchange(other.value_, nullptr)),
          destroy_(other.destroy_), install_(other.install_), code_(std::move(other.code_))
    {
    }

    DecodedComponent &DecodedComponent::operator=(DecodedComponent &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            type_ = other.type_;
            bytes_ = other.bytes_;
            value_ = std::exchange(other.value_, nullptr);
            destroy_ = other.destroy_;
            install_ = other.install_;
            code_ = std::move(other.code_);
        }
        return *this;
    }

    DecodedComponent::~DecodedComponent()
    {
        reset();
    }

    void DecodedComponent::reset() noexcept
    {
        if (value_)
        {
            destroy_(std::exchange(value_, nullptr));
        }
        // Destruction callback has returned to this library before its code lease can end.
        code_.reset();
    }

    lux::cxx::TypeToken DecodedComponent::type() const noexcept
    {
        return type_;
    }

    std::size_t DecodedComponent::accountedBytes() const noexcept
    {
        return value_ ? bytes_ : 0;
    }

    void DecodedComponent::installInto(Registry &registry, Entity entity) &&
    {
        assert(value_ && registry.valid(entity));
        install_(registry, entity, value_);
        reset();
    }
} // namespace lux::simulation::ecs
