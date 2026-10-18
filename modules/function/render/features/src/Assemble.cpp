#include <lux/engine/render/data/Encoding.hpp>
#include <lux/engine/render/features/Assembly.hpp>

#include <algorithm>
#include <numeric>
#include <tuple>

namespace lux::render
{
    namespace
    {
        RenderError failure(
            std::uint64_t type,
            std::size_t feature = 0,
            std::size_t resource = 0,
            std::size_t related = 0
        ) noexcept
        {
            return {type, {feature, resource, related}};
        }

        std::string prefix(FeatureName type, std::string_view instance) noexcept
        {
            return "feature:" + std::to_string(type.name().size()) + ":" + std::string(type.name()) + ":" +
                   std::to_string(instance.size()) + ":" + std::string(instance) + ":";
        }

        struct PlacedClaim final
        {
            ResourceClaim value;
            std::vector<GraphStorageBinding>* destination = nullptr;
        };

        // These are call-local authorship intervals, not another graph or owner.
        struct Contribution final
        {
            FeatureInstanceView selected;
            std::size_t input_index;
            std::vector<PlacedClaim> claims;
            std::vector<ResolvedResource> bindings;
            std::size_t first_resource = 0, last_resource = 0;
            std::size_t first_pass = 0, last_pass = 0;
            std::size_t first_output = 0, last_output = 0;
            std::size_t first_dependency = 0, last_dependency = 0;
        };

        struct ClaimLocation final
        {
            std::size_t feature;
            std::size_t claim;
        };

        struct SharedPort final
        {
            ClaimLocation provider;
            GraphVersion version;
        };

        auto rangeKey(const VGraphRange& range) noexcept
        {
            using Key = std::array<std::uint64_t, 6>;
            if (const auto* b = std::get_if<GraphBufferRange>(&range))
            {
                return Key{1, b->offset, b->size, 0, 0, 0};
            }
            const auto& i = std::get<GraphImageRange>(range);
            return Key{2, i.aspects, i.first_mip, i.mip_count, i.first_layer, i.layer_count};
        }

        void canonicalizeRanges(ResourceClaim& claim) noexcept
        {
            for (auto& range : claim.initial_ranges)
            {
                if (!std::holds_alternative<std::monostate>(range))
                {
                    continue;
                }
                if (const auto* buffer = std::get_if<GraphBufferDesc>(&claim.description))
                {
                    range = GraphBufferRange{0, buffer->size};
                }
                else
                {
                    const auto& image = std::get<GraphImageDesc>(claim.description);
                    range = GraphImageRange{rdesc::textureAspectMask(image.format), 0, image.mips, 0, image.layers};
                }
            }
            std::sort(
                claim.initial_ranges.begin(),
                claim.initial_ranges.end(),
                [](const auto& a, const auto& b) { return rangeKey(a) < rangeKey(b); }
            );
            auto end = std::unique(claim.initial_ranges.begin(), claim.initial_ranges.end());
            claim.initial_ranges.erase(end, claim.initial_ranges.end());
        }

        bool compatibleClaims(const ResourceClaim& a, const ResourceClaim& b) noexcept
        {
            return a.description == b.description && a.origin == b.origin && a.initial_ranges == b.initial_ranges &&
                   a.storage == b.storage;
        }

        RenderResult<void> validateInstances(std::vector<Contribution>& work, std::size_t config_byte_limit) noexcept
        {
            for (std::size_t i = 0; i < work.size(); ++i)
            {
                const auto& s = work[i].selected;
                if (!s.descriptor || !s.graph)
                {
                    return cxx::unexpected(failure(kFeatureContract, i));
                }
                const auto& d = *s.descriptor;
                const bool is_invalid_type = !d.type.isValid() || !s.graph->describe;
                const bool is_invalid_instance = s.instance_key.empty();
                const bool is_config_mismatch = d.config.token.name().empty() != (s.config == nullptr || !*s.config);
                if (is_invalid_type || is_invalid_instance || is_config_mismatch)
                {
                    return cxx::unexpected(failure(kFeatureContract, i));
                }
                if (s.config)
                {
                    if (d.config != s.config->typeInfo())
                    {
                        return cxx::unexpected(failure(kInvalidConfig, i));
                    }
                    if (s.config->retainedBytes() > config_byte_limit)
                    {
                        return cxx::unexpected(failure(kEncodingLimit, i));
                    }
                }
            }
            std::sort(
                work.begin(),
                work.end(),
                [](const auto& a, const auto& b)
                {
                    const auto& x = a.selected;
                    const auto& y = b.selected;
                    return std::tuple{x.descriptor->type.name(), x.instance_key} <
                           std::tuple{y.descriptor->type.name(), y.instance_key};
                }
            );
            for (std::size_t i = 0; i < work.size(); ++i)
            {
                const auto& s = work[i].selected;
                const bool is_same_type = i > 0 && work[i - 1].selected.descriptor->type == s.descriptor->type;
                if (is_same_type)
                {
                    const auto& previous = work[i - 1].selected;
                    const bool is_duplicate = previous.descriptor != s.descriptor || previous.graph != s.graph ||
                                              previous.instance_key == s.instance_key;
                    if (is_duplicate)
                    {
                        return cxx::unexpected(failure(kFeatureDuplicate, i, 0, i - 1));
                    }
                    continue;
                }
            }
            return {};
        }

        RenderResult<void> collectClaims(std::vector<Contribution>& work, GraphStorageBindings& external) noexcept
        {
            for (std::size_t i = 0; i < work.size(); ++i)
            {
                auto& w = work[i];
                const auto& s = w.selected;
                ResourceClaims declared;
                if (auto r = s.graph->describe(s.config ? s.config->data() : nullptr, s.instance_key, declared); !r)
                {
                    return r;
                }
                const auto append =
                    [&](std::vector<ResourceClaim>& source, std::vector<GraphStorageBinding>* destination)
                {
                    for (auto& claim : source)
                    {
                        w.claims.push_back({std::move(claim), destination});
                    }
                };
                append(declared.resources, nullptr);
                append(declared.storage.global, &external.global);
                append(declared.storage.scene, &external.scene);
                append(declared.storage.view, &external.view);
                append(declared.storage.attachment, &external.attachment);
                std::sort(
                    w.claims.begin(),
                    w.claims.end(),
                    [](const auto& a, const auto& b) { return a.value.key < b.value.key; }
                );
                for (std::size_t j = 0; j < w.claims.size(); ++j)
                {
                    auto& c = w.claims[j].value;
                    const bool is_invalid_claim = c.key.empty() || c.participation > EResourceClaim::OPTIONAL ||
                                                  c.origin > EGraphOrigin::HISTORY_CURRENT;
                    const bool is_invalid_placement = bool(c.storage) != (w.claims[j].destination != nullptr);
                    const bool is_invalid_storage =
                        is_invalid_placement ||
                        (c.storage && (!c.storage->type.isValid() || c.origin == EGraphOrigin::TRANSIENT));
                    if (is_invalid_claim || is_invalid_storage)
                    {
                        return cxx::unexpected(failure(kFeatureContract, i, j));
                    }
                    if (j > 0 && w.claims[j - 1].value.key == c.key)
                    {
                        return cxx::unexpected(failure(kFeatureDuplicate, i, j));
                    }
                    canonicalizeRanges(c);
                }
            }
            return {};
        }

        RenderResult<void> resolveClaims(
            std::vector<Contribution>& work,
            RenderGraphBuilder& builder,
            std::vector<SharedPort>& ports
        ) noexcept
        {
            std::vector<ClaimLocation> locations;
            for (std::size_t i = 0; i < work.size(); ++i)
            {
                for (std::size_t j = 0; j < work[i].claims.size(); ++j)
                {
                    locations.push_back({i, j});
                }
            }
            const auto claim = [&](ClaimLocation p) -> const ResourceClaim&
            { return work[p.feature].claims[p.claim].value; };
            const auto placement = [&](ClaimLocation p) { return work[p.feature].claims[p.claim].destination; };
            std::sort(
                locations.begin(),
                locations.end(),
                [&](auto a, auto b)
                { return std::tuple{claim(a).key, a.feature} < std::tuple{claim(b).key, b.feature}; }
            );
            for (std::size_t first = 0; first < locations.size();)
            {
                std::size_t end = first + 1;
                const auto& base = claim(locations[first]);
                while (end < locations.size() && claim(locations[end]).key == base.key)
                {
                    ++end;
                }
                std::optional<ClaimLocation> provider;
                bool required = false;
                for (auto i = first; i < end; ++i)
                {
                    const auto at = locations[i];
                    const auto& c = claim(at);
                    if (!compatibleClaims(base, c) || placement(locations[first]) != placement(at))
                    {
                        return cxx::unexpected(failure(kFeatureResourceMismatch, at.feature, at.claim));
                    }
                    required |= c.participation == EResourceClaim::REQUIRE;
                    if (c.participation != EResourceClaim::PROVIDE)
                    {
                        continue;
                    }
                    if (provider)
                    {
                        return cxx::unexpected(failure(kFeatureDuplicate, at.feature, at.claim, provider->feature));
                    }
                    provider = at;
                }
                if (!provider && required)
                {
                    return cxx::unexpected(failure(kFeatureMissingProvider, locations[first].feature, first));
                }
                if (provider)
                {
                    const auto& c = claim(*provider);
                    auto resource =
                        builder.declareResource({"shared:" + c.key, c.description, c.origin, c.initial_ranges});
                    if (!resource)
                    {
                        return cxx::unexpected(resource.error());
                    }
                    const bool is_initial = c.origin == EGraphOrigin::HISTORY_PREVIOUS ||
                                            (c.origin == EGraphOrigin::IMPORTED && !c.initial_ranges.empty());
                    const GraphVersion version{*resource, is_initial ? 0u : 1u};
                    ports.push_back({*provider, version});
                    if (c.storage)
                    {
                        placement(*provider)->push_back({*resource, *c.storage});
                    }
                    for (auto i = first; i < end; ++i)
                    {
                        work[locations[i].feature].bindings.push_back({c.key, version});
                    }
                }
                first = end;
            }
            return {};
        }

        RenderResult<void> audit(
            const RenderGraphDefinition& graph,
            const std::vector<Contribution>& work,
            const std::vector<SharedPort>& ports
        ) noexcept
        {
            for (std::size_t i = 0; i < work.size(); ++i)
            {
                const auto& w = work[i];
                const auto key_prefix = prefix(w.selected.descriptor->type, w.selected.instance_key);
                const auto has_name = [&](std::string_view key)
                { return key.starts_with(key_prefix) && key.size() > key_prefix.size(); };
                const auto check = [&](GraphVersion version, bool write) -> RenderResult<void>
                {
                    const auto r = version.resource.index;
                    if (r >= graph.resources().size())
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, r));
                    }
                    if (r >= ports.size())
                    {
                        if (r < w.first_resource || r >= w.last_resource)
                        {
                            return cxx::unexpected(failure(kFeatureOwnership, i, r));
                        }
                        return {};
                    }
                    const auto& port = ports[r];
                    const auto& c = work[port.provider.feature].claims[port.provider.claim].value;
                    if (!findResource(w.bindings, c.key))
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, r));
                    }
                    if (version != port.version)
                    {
                        return cxx::unexpected(failure(kFeatureVersion, i, r));
                    }
                    if (write && port.provider.feature != i)
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, r, port.provider.feature));
                    }
                    return {};
                };
                for (auto r = w.first_resource; r < w.last_resource; ++r)
                {
                    const auto& resource = graph.resources()[r];
                    const bool is_private_scratch =
                        has_name(resource.key) && resource.origin == EGraphOrigin::TRANSIENT;
                    if (!is_private_scratch)
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, r));
                    }
                }
                for (auto p = w.first_pass; p < w.last_pass; ++p)
                {
                    const auto& pass = graph.passes()[p];
                    if (!has_name(pass.key))
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, 0, p));
                    }
                    for (const auto& use : pass.uses)
                    {
                        const bool writes = use.access != EGraphAccess::READ;
                        if (auto r = check(use.version, writes); !r)
                        {
                            return r;
                        }
                        const bool reads_predecessor =
                            use.access == EGraphAccess::READ_WRITE || use.access == EGraphAccess::ATTACHMENT_LOAD;
                        if (reads_predecessor)
                        {
                            if (auto r = check({use.version.resource, use.input_version}, false); !r)
                            {
                                return r;
                            }
                        }
                        if (use.fallback)
                        {
                            if (auto r = check(*use.fallback, false); !r)
                            {
                                return r;
                            }
                        }
                    }
                }
                for (auto o = w.first_output; o < w.last_output; ++o)
                {
                    // Exporting observes a version; require PROVIDE authority, even for v0.
                    if (auto r = check(graph.outputs()[o].version, true); !r)
                    {
                        return r;
                    }
                }
                // BOUNDARY: audit only newly appended edges. Builder deduplicates no-op requests.
                for (auto d = w.first_dependency; d < w.last_dependency; ++d)
                {
                    const auto& edge = graph.dependencies()[d];
                    const bool owns_before = edge.before.index >= w.first_pass && edge.before.index < w.last_pass;
                    const bool owns_after = edge.after.index >= w.first_pass && edge.after.index < w.last_pass;
                    if (!owns_before || !owns_after)
                    {
                        return cxx::unexpected(failure(kFeatureOwnership, i, edge.before.index, edge.after.index));
                    }
                }
            }
            return {};
        }
    } // namespace

    std::string featureKey(FeatureName type, std::string_view instance, std::string_view local) noexcept
    {
        return prefix(type, instance) + std::string(local);
    }

    const ResolvedResource* findResource(std::span<const ResolvedResource> resolved, std::string_view key) noexcept
    {
        auto at = std::lower_bound(
            resolved.begin(),
            resolved.end(),
            key,
            [](const auto& binding, auto name) { return binding.key < name; }
        );
        return at != resolved.end() && at->key == key ? &*at : nullptr;
    }

    RenderResult<FeatureAssembly> assembleFeatures(
        std::span<const FeatureInstanceView> selected,
        std::size_t config_byte_limit
    ) noexcept
    {
        std::vector<Contribution> work;
        work.reserve(selected.size());
        for (std::size_t i = 0; i < selected.size(); ++i)
        {
            work.push_back({selected[i], i});
        }
        if (auto r = validateInstances(work, config_byte_limit); !r)
        {
            return cxx::unexpected(r.error());
        }
        GraphStorageBindings external;
        if (auto r = collectClaims(work, external); !r)
        {
            return cxx::unexpected(r.error());
        }
        RenderGraphBuilder builder;
        std::vector<SharedPort> ports;
        if (auto r = resolveClaims(work, builder, ports); !r)
        {
            return cxx::unexpected(r.error());
        }
        for (auto& w : work)
        {
            w.first_resource = builder.resourceCount();
            w.first_pass = builder.passCount();
            w.first_output = builder.outputCount();
            w.first_dependency = builder.dependencyCount();
            const auto& s = w.selected;
            if (s.graph->build)
            {
                auto r = s.graph->build(s.config ? s.config->data() : nullptr, s.instance_key, w.bindings, builder);
                if (!r)
                {
                    return cxx::unexpected(r.error());
                }
            }
            w.last_resource = builder.resourceCount();
            w.last_pass = builder.passCount();
            w.last_output = builder.outputCount();
            w.last_dependency = builder.dependencyCount();
            const bool is_replaced_builder = w.last_resource < w.first_resource || w.last_pass < w.first_pass ||
                                             w.last_output < w.first_output || w.last_dependency < w.first_dependency;
            if (is_replaced_builder)
            {
                return cxx::unexpected(failure(kFeatureContract));
            }
        }
        auto graph = std::move(builder).finish();
        if (!graph)
        {
            return cxx::unexpected(graph.error());
        }
        if (auto r = audit(*graph, work, ports); !r)
        {
            return cxx::unexpected(r.error());
        }
        auto analysis = analyze(*graph);
        if (!analysis)
        {
            return cxx::unexpected(analysis.error());
        }
        std::vector<FeatureContribution> contributions;
        contributions.reserve(work.size());
        for (const auto& w : work)
        {
            contributions.push_back({w.input_index, w.first_pass, w.last_pass - w.first_pass});
        }
        return FeatureAssembly{std::move(*graph), std::move(*analysis), std::move(external), std::move(contributions)};
    }

    std::span<const error::ErrorDescriptor> renderFeatureErrorDescriptors() noexcept
    {
        using error::EArgument;
        static constexpr auto args = std::array{EArgument::UNSIGNED, EArgument::UNSIGNED, EArgument::UNSIGNED};
        static constexpr error::ErrorDescriptor descriptors[]{
            {"lux.render.feature.contract", "Invalid feature contract", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.duplicate", "Duplicate feature declaration", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.missing_provider", "Required provider missing", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.resource_mismatch", "Resource contracts differ", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.ownership", "Unauthorized graph contribution", error::ERecovery::PERMANENT, args},
            {"lux.render.feature.version", "Unsupported shared version", error::ERecovery::PERMANENT, args}
        };
        return descriptors;
    }
} // namespace lux::render
