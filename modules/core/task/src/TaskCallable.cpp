#include <lux/engine/task/TaskCallable.hpp>

#include <utility>

namespace lux::task
{
    TaskCallable::~TaskCallable() noexcept
    {
        reset();
    }

    TaskCallable::TaskCallable(TaskCallable&& other) noexcept
        : object_(std::exchange(other.object_, nullptr)), invoke_(std::exchange(other.invoke_, nullptr)),
          destroy_(std::exchange(other.destroy_, nullptr))
    {
    }

    TaskCallable& TaskCallable::operator=(TaskCallable&& other) noexcept
    {
        if (this == std::addressof(other))
            return *this;
        reset();
        object_ = std::exchange(other.object_, nullptr);
        invoke_ = std::exchange(other.invoke_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        return *this;
    }

    void TaskCallable::reset() noexcept
    {
        if (object_ != nullptr)
            destroy_(object_);
        object_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
    }
}
