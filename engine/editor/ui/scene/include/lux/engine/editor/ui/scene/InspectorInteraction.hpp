#pragma once
#include <lux/engine/ui/Frame.hpp>
#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace lux::editor::ui
{
    // Pane-owned input scratch only. No author model, Registry, history or backend context.
    struct InspectorInteraction final
    {
        struct Scratch
        {
            virtual ~Scratch() = default;
        };
        template<class T> struct Value final : Scratch
        {
            T value{};
        };
        struct Slot final
        {
            std::uint64_t id;
            lux::cxx::TypeToken type;
            std::unique_ptr<Scratch> owner;
        };
        std::vector<Slot> scratch;
        std::array<char, 192> error{};
        bool read_only{};
        void *asset_source{};
        std::string (*asset_path)(void *, lux::asset::AssetId){};

        template<class T> T &input(std::uint64_t id)
        {
            const auto type = lux::cxx::typeToken<T>();
            for (auto &slot : scratch)
                if (slot.id == id && slot.type == type)
                    return static_cast<Value<T> &>(*slot.owner).value;
            auto owner = std::make_unique<Value<T>>();
            auto &value = owner->value;
            scratch.push_back({id, type, std::move(owner)});
            return value;
        }
        void fail(const char *message) noexcept
        {
            std::size_t i{};
            while (message[i] && i + 1 < error.size())
            {
                error[i] = message[i];
                ++i;
            }
            error[i] = '\0';
        }
        void reset() noexcept
        {
            scratch.clear();
            error.fill('\0');
        }
    };
} // namespace lux::editor::ui
