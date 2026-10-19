#pragma once

#include <lux/engine/render/native/vulkan/Feature.hpp>

#include <lux/engine/render/runtime/RenderServer.hpp>
#include <map>

namespace lux::render
{
    struct RenderServer::HandlerRecord final
    {
        CppType type;
        std::string name;
        std::string instance_name;
        std::unique_ptr<void, void (*)(void*) noexcept> object{nullptr, [](void*) noexcept {}};
    };

    struct RenderServer::RegistrationBatch::State final
    {
        struct Handler final
        {
            std::unique_ptr<HandlerRecord> record;
            std::vector<CommandBinding> calls;
        };

        struct Storage final
        {
            StorageFactory factory;
            ConfigValue configuration;
            bool use_existing_configuration;
            StorageTypeId id;
        };

        struct Feature final
        {
            feature_detail::Implementation definition;
            FeatureId id;
        };

        using HandlerName = std::pair<std::string_view, std::string_view>;
        std::vector<Handler> handlers;
        std::map<HandlerName, std::size_t> handlers_by_name;
        std::vector<Storage> storage_factories;
        std::vector<Feature> features;
    };
} // namespace lux::render
