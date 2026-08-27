#pragma once

#include <lux/engine/simulation/ecs/Entity.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::simulation
{
    enum class ESystemEventBufferError : std::uint8_t
    {
        NOT_PREPARED,
        INVALID_PRODUCER,
        ACTIVE_WRITER,
        CAPACITY_EXCEEDED,
        RECORDING_FAILED,
        ALLOCATION_FAILURE,
    };

    /**
     * System-owned future-frame storage for typed event values.
     *
     * Each worker writes only its reserved producer-local vector. Draining is
     * a synchronous safe-point operation and visits producer ordinals, then
     * local append order. The buffer never stores a call frame or a pointer to
     * caller-owned payload memory.
     */
    template <class Payload> class SystemEventBuffer final
    {
        static_assert(std::is_nothrow_destructible_v<Payload>);

    public:
        struct Occurrence final
        {
            ecs::Entity target{ecs::NullEntity};
            Payload payload;
        };

    private:
        struct Producer final
        {
            std::vector<Occurrence> occurrences;
            bool active{};
            bool failed{};
        };

    public:
        class Writer final
        {
        public:
            Writer() noexcept = default;
            Writer(Writer&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), producer_(other.producer_)
            {
            }
            Writer& operator=(Writer&& other) noexcept
            {
                if (this != std::addressof(other))
                {
                    close();
                    owner_ = std::exchange(other.owner_, nullptr);
                    producer_ = other.producer_;
                }
                return *this;
            }
            Writer(const Writer&) = delete;
            Writer& operator=(const Writer&) = delete;
            ~Writer() noexcept
            {
                close();
            }

            [[nodiscard]] lux::cxx::expected<void, ESystemEventBufferError>
            emit(ecs::Entity target, const Payload& payload) noexcept
            {
                if (!owner_)
                {
                    return lux::cxx::unexpected(ESystemEventBufferError::INVALID_PRODUCER);
                }
                auto& producer = owner_->producers_[producer_];
                if (producer.failed)
                {
                    return lux::cxx::unexpected(ESystemEventBufferError::RECORDING_FAILED);
                }
                if (producer.occurrences.size() >= owner_->capacity_per_producer_)
                {
                    producer.failed = true;
                    return lux::cxx::unexpected(ESystemEventBufferError::CAPACITY_EXCEEDED);
                }
                try
                {
                    producer.occurrences.push_back(Occurrence{target, payload});
                    return {};
                }
                catch (...)
                {
                    producer.failed = true;
                    return lux::cxx::unexpected(ESystemEventBufferError::RECORDING_FAILED);
                }
            }

            [[nodiscard]] lux::cxx::expected<void, ESystemEventBufferError>
            emit(ecs::Entity target, Payload&& payload) noexcept
                requires std::is_nothrow_move_constructible_v<Payload>
            {
                if (!owner_)
                {
                    return lux::cxx::unexpected(ESystemEventBufferError::INVALID_PRODUCER);
                }
                auto& producer = owner_->producers_[producer_];
                if (producer.failed)
                {
                    return lux::cxx::unexpected(ESystemEventBufferError::RECORDING_FAILED);
                }
                if (producer.occurrences.size() >= owner_->capacity_per_producer_)
                {
                    producer.failed = true;
                    return lux::cxx::unexpected(ESystemEventBufferError::CAPACITY_EXCEEDED);
                }
                producer.occurrences.push_back(Occurrence{target, std::move(payload)});
                return {};
            }

        private:
            Writer(SystemEventBuffer& owner, std::size_t producer) noexcept
                : owner_(std::addressof(owner)), producer_(producer)
            {
            }

            void close() noexcept
            {
                if (owner_)
                {
                    owner_->producers_[producer_].active = false;
                    owner_ = nullptr;
                }
            }

            SystemEventBuffer* owner_{};
            std::size_t producer_{};
            friend class SystemEventBuffer;
        };

        SystemEventBuffer() noexcept = default;
        SystemEventBuffer(SystemEventBuffer&&) = delete;
        SystemEventBuffer& operator=(SystemEventBuffer&&) = delete;
        SystemEventBuffer(const SystemEventBuffer&) = delete;
        SystemEventBuffer& operator=(const SystemEventBuffer&) = delete;

        [[nodiscard]] lux::cxx::expected<void, ESystemEventBufferError>
        prepare(std::size_t producer_count, std::size_t capacity_per_producer) noexcept
        {
            if (hasActiveWriter())
            {
                return lux::cxx::unexpected(ESystemEventBufferError::ACTIVE_WRITER);
            }
            if (producer_count == 0U || capacity_per_producer == 0U)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::CAPACITY_EXCEEDED);
            }
            try
            {
                auto prepared = std::make_unique<Producer[]>(producer_count);
                for (std::size_t index{}; index < producer_count; ++index)
                    prepared[index].occurrences.reserve(capacity_per_producer);
                producers_ = std::move(prepared);
                producer_count_ = producer_count;
                capacity_per_producer_ = capacity_per_producer;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::ALLOCATION_FAILURE);
            }
        }

        [[nodiscard]] lux::cxx::expected<Writer, ESystemEventBufferError> writer(std::size_t producer) noexcept
        {
            if (!producers_)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::NOT_PREPARED);
            }
            if (producer >= producer_count_)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::INVALID_PRODUCER);
            }
            if (producers_[producer].active)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::ACTIVE_WRITER);
            }
            producers_[producer].active = true;
            return Writer{*this, producer};
        }

        template <class Callback>
        [[nodiscard]] lux::cxx::expected<void, ESystemEventBufferError> drain(Callback&& callback) noexcept
        {
            static_assert(std::is_nothrow_invocable_v<Callback&, ecs::Entity, const Payload&>);
            if (!producers_)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::NOT_PREPARED);
            }
            if (hasActiveWriter())
            {
                return lux::cxx::unexpected(ESystemEventBufferError::ACTIVE_WRITER);
            }
            for (std::size_t producer{}; producer < producer_count_; ++producer)
            {
                if (producers_[producer].failed)
                {
                    return lux::cxx::unexpected(ESystemEventBufferError::RECORDING_FAILED);
                }
            }
            for (std::size_t producer{}; producer < producer_count_; ++producer)
            {
                for (const auto& occurrence : producers_[producer].occurrences)
                {
                    callback(occurrence.target, occurrence.payload);
                }
                producers_[producer].occurrences.clear();
            }
            return {};
        }

        [[nodiscard]] lux::cxx::expected<void, ESystemEventBufferError> reset() noexcept
        {
            if (!producers_)
            {
                return lux::cxx::unexpected(ESystemEventBufferError::NOT_PREPARED);
            }
            if (hasActiveWriter())
            {
                return lux::cxx::unexpected(ESystemEventBufferError::ACTIVE_WRITER);
            }
            for (std::size_t producer{}; producer < producer_count_; ++producer)
            {
                producers_[producer].occurrences.clear();
                producers_[producer].failed = false;
            }
            return {};
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            std::size_t result{};
            for (std::size_t producer{}; producer < producer_count_; ++producer)
            {
                result += producers_[producer].occurrences.size();
            }
            return result;
        }

    private:
        [[nodiscard]] bool hasActiveWriter() const noexcept
        {
            for (std::size_t producer{}; producer < producer_count_; ++producer)
            {
                if (producers_[producer].active)
                    return true;
            }
            return false;
        }

        std::unique_ptr<Producer[]> producers_;
        std::size_t producer_count_{};
        std::size_t capacity_per_producer_{};
    };
}
