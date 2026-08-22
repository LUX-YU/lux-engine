    lux::cxx::expected<WorldActorDocument, std::string>
    loadWorldActorDocument(
        const std::filesystem::path& root_document,
        const WorldActorSourceDescriptor& descriptor,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto resolved = resolveWorldSourceDocument(
            root_document, descriptor.document_path);
        if (!resolved)
            return lux::cxx::unexpected(std::move(resolved.error()));
        auto bytes = readFile(*resolved, limits.maximum_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        if (lux::cxx::algorithm::Sha256::hash(*bytes) != descriptor.content_digest)
        {
            return lux::cxx::unexpected(
                std::string{"Actor document digest mismatch"});
        }
        return decodeWorldActorDocument(*bytes, limits);
    }

    lux::cxx::expected<void, std::string> saveWorldSource(
        const std::filesystem::path& path,
        const WorldSourceDocument& document) noexcept
    {
        auto bytes = encodeWorldSource(document);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        return atomicWrite(path, *bytes);
    }

    lux::cxx::expected<WorldSourceDocument, std::string> loadWorldSource(
        const std::filesystem::path& path,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto bytes = readFile(path, limits.maximum_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        auto decoded = decodeWorldSource(*bytes, limits);
        if (!decoded)
            return lux::cxx::unexpected(std::move(decoded.error().detail));
        return std::move(*decoded);
    }

    lux::cxx::expected<WorldDescriptorPageDocument, std::string>
    loadWorldDescriptorPage(
        const std::filesystem::path& root_document,
        const WorldSourceDocument& root,
        const WorldDescriptorPageReference& reference,
        const WorldSourceCodecLimits& limits) noexcept
    {
        auto path = resolveWorldSourceDocument(
            root_document, reference.document_path);
        if (!path)
            return lux::cxx::unexpected(std::move(path.error()));
        auto bytes = readFile(
            *path,
            limits.maximum_descriptor_page_bytes);
        if (!bytes)
            return lux::cxx::unexpected(std::move(bytes.error()));
        if (lux::cxx::algorithm::Sha256::hash(*bytes) != reference.content_digest)
            return lux::cxx::unexpected(
                std::string{"Descriptor Page digest mismatch"});
        auto page = decodeWorldDescriptorPage(root, *bytes, limits);
        if (!page)
            return lux::cxx::unexpected(std::move(page.error()));
        if (page->id != reference.id || page->space != reference.space
            || page->macro != reference.macro
            || page->actors.size() != reference.actor_count
            || page->pages.size() != reference.page_count)
            return lux::cxx::unexpected(
                std::string{"Descriptor Page does not match Root reference"});
        return page;
    }

    lux::cxx::expected<std::filesystem::path, std::string>
    resolveWorldSourceDocument(
        const std::filesystem::path& root_document,
        std::string_view relative_path) noexcept
    {
        if (!validPath(relative_path))
            return lux::cxx::unexpected(
                std::string{"invalid external World document path"});
        std::error_code error;
        const auto root = canonicalizeExistingPrefix(
            root_document.parent_path(), error);
        if (error)
            return lux::cxx::unexpected(
                std::string{"cannot resolve World source root: "}
                + error.message());
        const auto candidate = canonicalizeExistingPrefix(
            root / std::filesystem::path{relative_path}, error);
        if (error)
        {
            return lux::cxx::unexpected(
                std::string{"cannot resolve external World document: "}
                + error.message());
        }
        if (!confinedTo(root, candidate))
            return lux::cxx::unexpected(
                std::string{"external World document escapes its root"});
        return candidate;
    }

    lux::cxx::expected<void, std::string> saveWorldSourceDocument(
        const std::filesystem::path& root_document,
        std::string_view relative_path,
        std::span<const std::byte> bytes) noexcept
    {
        auto path = resolveWorldSourceDocument(root_document, relative_path);
        if (!path)
            return lux::cxx::unexpected(std::move(path.error()));
        return atomicWrite(*path, bytes);
    }

    lux::cxx::expected<WorldSourceGarbageCollectionResult, std::string>
    collectWorldSourceGarbage(
        const std::filesystem::path& root_document,
        const WorldSourceGarbageCollectionConfig& config,
        const WorldSourceCodecLimits& limits) noexcept
    {
        if (config.grace_period < std::chrono::seconds::zero() ||
            config.maximum_removals_per_pass == 0u)
        {
            return lux::cxx::unexpected(
                std::string{"invalid World source garbage collection config"});
        }
        auto root = loadWorldSource(root_document, limits);
        if (!root)
            return lux::cxx::unexpected(std::move(root.error()));

        // Finish the complete, digest-verified live-set before looking at any
        // deletion candidate. A broken current Root can therefore never turn
        // this maintenance pass into a partial destructive transaction.
        std::unordered_set<std::string> live;
        live.reserve(root->descriptor_pages.size() * 2u);
        for (const auto& reference : root->descriptor_pages)
        {
            auto page = loadWorldDescriptorPage(
                root_document, *root, reference, limits);
            if (!page)
                return lux::cxx::unexpected(std::move(page.error()));
            live.insert(reference.document_path);
            for (const auto& actor : page->actors)
                live.insert(actor.document_path);
            for (const auto& content : page->pages)
                live.insert(content.document_path);
        }

        WorldSourceGarbageCollectionResult result;
        result.live_documents = live.size();
        std::error_code error;
        const auto source_root = std::filesystem::weakly_canonical(
            root_document.parent_path(), error);
        if (error)
        {
            return lux::cxx::unexpected(
                std::string{"cannot canonicalize World source root: "} +
                error.message());
        }

        static constexpr std::array<std::string_view, 6u> directories{
            "Actors", "Descriptors", "Instances",
            "Terrain", "Tiles", "Pixels"};
        static constexpr std::array<std::string_view, 6u> extensions{
            ".lxad", ".lxai", ".lxip", ".lxtp", ".lxtl", ".lxpp"};
        struct Candidate final
        {
            std::filesystem::path path;
            bool old_enough{false};
        };
        std::vector<Candidate> candidates;
        const auto now = std::filesystem::file_time_type::clock::now();
        for (const auto directory_name : directories)
        {
            const auto directory = source_root /
                std::filesystem::path{directory_name};
            if (!std::filesystem::exists(directory, error))
            {
                if (error)
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot inspect World source object root: "}
                        + error.message());
                }
                continue;
            }
            std::filesystem::recursive_directory_iterator iterator{
                directory,
                std::filesystem::directory_options::skip_permission_denied,
                error};
            if (error)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot scan World source objects: "} +
                    error.message());
            }
            const std::filesystem::recursive_directory_iterator end;
            for (; iterator != end; iterator.increment(error))
            {
                if (error)
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot continue World source scan: "} +
                        error.message());
                }
                const auto status = iterator->symlink_status(error);
                if (error)
                    return lux::cxx::unexpected(error.message());
                if (std::filesystem::is_symlink(status) ||
                    !std::filesystem::is_regular_file(status) ||
                    std::ranges::find(
                        extensions,
                        iterator->path().extension().generic_string()) ==
                        extensions.end())
                {
                    continue;
                }
                ++result.scanned_documents;
                const auto relative = iterator->path()
                    .lexically_relative(source_root).generic_string();
                if (live.contains(relative))
                    continue;
                const auto modified = iterator->last_write_time(error);
                if (error)
                    return lux::cxx::unexpected(error.message());
                const bool old_enough = now >= modified &&
                    now - modified >= config.grace_period;
                candidates.push_back({iterator->path(), old_enough});
                if (!old_enough)
                    ++result.deferred_documents;
            }
        }

        for (const auto& candidate : candidates)
        {
            if (!candidate.old_enough)
                continue;
            if (result.removed_documents >=
                config.maximum_removals_per_pass)
            {
                result.removal_budget_exhausted = true;
                ++result.deferred_documents;
                continue;
            }
            const auto canonical = std::filesystem::weakly_canonical(
                candidate.path, error);
            if (error || !confinedTo(source_root, canonical))
            {
                return lux::cxx::unexpected(
                    std::string{"World source GC candidate escaped its root"});
            }
            if (!std::filesystem::remove(canonical, error) || error)
            {
                return lux::cxx::unexpected(
                    std::string{"cannot remove orphan World source object: "}
                    + error.message());
            }
            ++result.removed_documents;
        }
        return result;
    }
