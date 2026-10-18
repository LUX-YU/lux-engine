#pragma once

#include <lux/engine/render/data/ConfigValue.hpp>
#include <lux/engine/render/resource/Factory.hpp>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace lux::render
{
    class ResourceRegistry;

    // The Device/Scene/View/Attachment exclusively owns this set. Accepted operations borrow its instances.
    class StorageSet final
    {
    public:
        explicit StorageSet(ResourceRegistry& registry) noexcept : registry_(registry) {}

        ~StorageSet() noexcept;
        StorageSet(const StorageSet&) = delete;
        StorageSet& operator=(const StorageSet&) = delete;
        StorageSet(StorageSet&&) = delete;
        StorageSet& operator=(StorageSet&&) = delete;

        template <class T>
        [[nodiscard]] RenderResult<StorageId> getOrCreate(std::string_view key, ConfigValue config = {}) noexcept
        {
            return getOrCreateImpl(
                T::description,
                key,
                std::move(config),
                cppType<typename T::Config>(),
                cppType<typename T::Ops>()
            );
        }

        [[nodiscard]] RenderResult<StorageId> getOrCreate(
            StorageTypeId type,
            std::string_view key,
            ConfigValue config = {}
        ) noexcept;

        // Caller finishes dependent operations before removal. The ID is revoked before destruction.
        [[nodiscard]] RenderResult<void> remove(StorageId id) noexcept;
        [[nodiscard]] StorageId find(StorageTypeId type, std::string_view key) const noexcept;

        [[nodiscard]] ResourceRegistry& registry() const noexcept
        {
            return registry_;
        }

        [[nodiscard]] std::size_t size() const noexcept
        {
            return records_.size();
        }

        void clear() noexcept;

    private:
        friend class ResourceRegistry;
        using NameKey = std::tuple<std::uint32_t, std::uint32_t, std::string>;
        using NameIndex = std::map<NameKey, StorageId, std::less<>>;

        struct Record final
        {
            StorageId id;
            NameIndex::iterator name;
            ConfigValue configuration;
            NativeStorage native;
        };

        [[nodiscard]] RenderResult<StorageId> getOrCreateImpl(
            const StorageDescriptor&,
            std::string_view,
            ConfigValue,
            CppType,
            CppType
        ) noexcept;
        [[nodiscard]] RenderResult<
            StorageId> ensure(StorageTypeId, std::string_view, ConfigValue, CppType = {}, CppType = {}) noexcept;
        void erase(std::size_t position) noexcept;
        ResourceRegistry& registry_;
        std::vector<Record> records_;
        NameIndex instances_by_name_;
    };
} // namespace lux::render
