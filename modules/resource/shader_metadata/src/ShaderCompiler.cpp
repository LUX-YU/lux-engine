#include <lux/engine/shader/ShaderCompiler.hpp>

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#undef interface
#else
#include <dlfcn.h>
#endif

namespace lux::shader
{
    namespace
    {
        using Kind = slang::TypeReflection::Kind;
        using Layout = slang::TypeLayoutReflection;

        auto failure(EShaderCompileError code, std::string diagnostic)
        {
            return cxx::unexpected(ShaderCompileFailure{code, std::move(diagnostic)});
        }

        std::string diagnostics(slang::IBlob* blob, const char* fallback)
        {
            return blob ? std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize())
                        : std::string(fallback);
        }

        std::filesystem::path runtimeDirectory()
        {
#ifdef _WIN32
            HMODULE module{};
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&compilerVersion),
                    &module
                ))
            {
                return {};
            }
            std::wstring path(32768, L'\0');
            const auto size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (size == 0 || size >= path.size())
            {
                return {};
            }
            path.resize(size);
            return std::filesystem::path(path).parent_path();
#else
            Dl_info info{};
            if (!dladdr(reinterpret_cast<const void*>(&compilerVersion), &info) || !info.dli_fname)
            {
                return {};
            }
            return std::filesystem::path(info.dli_fname).parent_path();
#endif
        }

        bool scalar(Layout& layout, rdesc::EDataScalar& result) noexcept
        {
            switch (layout.getScalarType())
            {
            case slang::TypeReflection::Float32:
                result = rdesc::EDataScalar::FLOAT;
                return true;
            case slang::TypeReflection::Int32:
                result = rdesc::EDataScalar::SINT;
                return true;
            case slang::TypeReflection::UInt32:
                result = rdesc::EDataScalar::UINT;
                return true;
            default:
                return false;
            }
        }

        bool
        fields(
            Layout& layout,
            std::string path,
            std::size_t offset,
            rdesc::ShaderBlock& block,
            std::uint32_t array_stride = 0,
            unsigned depth = 0
        )
        {
            if (depth > 32 || offset > UINT32_MAX)
            {
                return false;
            }
            if (layout.getKind() == Kind::Struct)
            {
                for (unsigned index = 0; index < layout.getFieldCount(); ++index)
                {
                    auto* field = layout.getFieldByIndex(index);
                    const auto field_offset = field->getOffset();
                    if (field_offset > UINT32_MAX - offset)
                    {
                        return false;
                    }
                    auto name = path.empty() ? std::string(field->getName()) : path + "." + field->getName();
                    if (!fields(
                            *field->getTypeLayout(),
                            std::move(name),
                            offset + field_offset,
                            block,
                            array_stride,
                            depth + 1
                        ))
                    {
                        return false;
                    }
                }
                return true;
            }
            if (layout.getKind() == Kind::Array)
            {
                const auto count = layout.getElementCount();
                const auto stride = layout.getElementStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
                const bool is_invalid_array =
                    count > UINT32_MAX || stride > UINT32_MAX || stride == 0 || count > (UINT32_MAX - offset) / stride;
                if (is_invalid_array)
                {
                    return false;
                }
                for (std::size_t index = 0; index < count; ++index)
                {
                    if (!fields(
                            *layout.getElementTypeLayout(),
                            path + "[" + std::to_string(index) + "]",
                            offset + index * stride,
                            block,
                            static_cast<std::uint32_t>(stride),
                            depth + 1
                        ))
                    {
                        return false;
                    }
                }
                return true;
            }
            rdesc::ShaderMember member{};
            if (!scalar(layout, member.scalar))
            {
                return false;
            }
            member.path = std::move(path);
            member.offset = static_cast<std::uint32_t>(offset);
            member.rows = std::max(layout.getRowCount(), 1U);
            member.columns = std::max(layout.getColumnCount(), 1U);
            if (layout.getKind() == Kind::Vector)
            {
                member.rows = member.columns;
                member.columns = 1;
            }
            member.array_stride = array_stride;
            if (layout.getKind() == Kind::Matrix)
            {
                if (layout.getMatrixLayoutMode() != SLANG_MATRIX_LAYOUT_COLUMN_MAJOR)
                {
                    return false;
                }
                // Slang's array-element query returns zero for a matrix. Its complete uniform stride
                // covers the column vectors, including target-specific trailing padding.
                const auto matrix_extent = layout.getStride(SLANG_PARAMETER_CATEGORY_UNIFORM);
                if (!member.columns || matrix_extent > UINT32_MAX || matrix_extent % member.columns != 0)
                {
                    return false;
                }
                member.matrix_stride = static_cast<std::uint32_t>(matrix_extent / member.columns);
            }
            block.fields.push_back(std::move(member));
            return true;
        }

        bool blockLayout(Layout* layout, bool records, rdesc::ShaderBlock& block)
        {
            if (!layout)
            {
                return false;
            }
            const auto size = records ? layout->getStride() : layout->getSize();
            const auto alignment = layout->getAlignment();
            if (size > UINT32_MAX || alignment <= 0)
            {
                return false;
            }
            block.size = static_cast<std::uint32_t>(size);
            block.alignment = static_cast<std::uint32_t>(alignment);
            block.storage = rdesc::EDataLayout::TARGET_REFLECTED;
            block.runtime_records = records;
            return fields(*layout, {}, 0, block);
        }

        bool resource(slang::VariableLayoutReflection& variable, std::uint32_t stages, rdesc::ShaderInfo& info)
        {
            auto* declared = variable.getTypeLayout();
            auto* type = declared->unwrapArray();
            const auto kind = type->getKind();
            rdesc::EDescriptorBindingInfo binding;
            binding.name = variable.getName();
            binding.set = variable.getBindingSpace();
            binding.binding = variable.getBindingIndex();
            binding.stages = stages;
            const auto count = declared->isArray() ? declared->getTotalArrayElementCount() : 1;
            if (count != SLANG_UNBOUNDED_SIZE && count > UINT32_MAX)
            {
                return false;
            }
            binding.count = count == SLANG_UNBOUNDED_SIZE ? 0 : static_cast<std::uint32_t>(count);
            if (kind == Kind::ConstantBuffer)
            {
                binding.type = rdesc::EDescriptorType::UNIFORM_BUFFER;
                if (!blockLayout(type->getElementTypeLayout(), false, binding.block))
                {
                    return false;
                }
            }
            else if (kind == Kind::SamplerState)
            {
                binding.type = rdesc::EDescriptorType::SAMPLER;
            }
            else if (kind == Kind::Resource || kind == Kind::ShaderStorageBuffer)
            {
                const auto shape = type->getResourceShape() & SLANG_RESOURCE_BASE_SHAPE_MASK;
                const auto access = type->getResourceAccess();
                binding.writable = access == SLANG_RESOURCE_ACCESS_READ_WRITE || access == SLANG_RESOURCE_ACCESS_WRITE;
                binding.readable = access != SLANG_RESOURCE_ACCESS_WRITE;
                const bool has_supported_access = access == SLANG_RESOURCE_ACCESS_READ || binding.writable;
                if (!has_supported_access)
                {
                    return false;
                }
                if (shape == SLANG_STRUCTURED_BUFFER || shape == SLANG_BYTE_ADDRESS_BUFFER)
                {
                    binding.type = rdesc::EDescriptorType::STORAGE_BUFFER;
                    if (shape == SLANG_STRUCTURED_BUFFER &&
                        !blockLayout(type->getElementTypeLayout(), true, binding.block))
                    {
                        return false;
                    }
                }
                else
                {
                    const bool is_texture = shape == SLANG_TEXTURE_1D || shape == SLANG_TEXTURE_2D ||
                                            shape == SLANG_TEXTURE_3D || shape == SLANG_TEXTURE_CUBE;
                    if (!is_texture)
                    {
                        return false;
                    }
                    binding.type = binding.writable ? rdesc::EDescriptorType::STORAGE_IMAGE
                                                    : rdesc::EDescriptorType::SAMPLED_IMAGE;
                    binding.image_arrayed = (type->getResourceShape() & SLANG_TEXTURE_ARRAY_FLAG) != 0;
                }
            }
            else
            {
                return false;
            }
            bool is_push_constant = binding.set == 255 && binding.binding == 0;
            for (unsigned category = 0; category < variable.getCategoryCount(); ++category)
            {
                is_push_constant |=
                    variable.getCategoryByIndex(category) == slang::ParameterCategory::PushConstantBuffer;
            }
            if (kind == Kind::ConstantBuffer && is_push_constant)
            {
                info.push_constants.push_back({0, binding.block.size, std::move(binding.block), stages});
                return true;
            }
            binding.blockSize = binding.block.size;
            auto set = std::ranges::find(info.sets, binding.set, &rdesc::DescriptorSetLayoutInfo::set);
            if (set == info.sets.end())
            {
                info.sets.push_back({binding.set, {}});
                set = std::prev(info.sets.end());
            }
            const bool has_collision = std::ranges::any_of(
                set->bindings,
                [&](const auto& existing) { return existing.binding == binding.binding; }
            );
            if (has_collision)
            {
                return false;
            }
            set->bindings.push_back(std::move(binding));
            return true;
        }

        bool
        stageIo(
            slang::VariableLayoutReflection& variable,
            std::vector<rdesc::ShaderIoVariable>& result,
            std::size_t base = 0
        )
        {
            auto* type = variable.getTypeLayout();
            const auto category = variable.getCategory();
            if (type->getKind() == Kind::Struct)
            {
                for (unsigned index = 0; index < type->getFieldCount(); ++index)
                {
                    if (!stageIo(*type->getFieldByIndex(index), result, base + variable.getOffset(category)))
                    {
                        return false;
                    }
                }
                return true;
            }
            const char* semantic = variable.getSemanticName();
            std::string semantic_name = semantic ? semantic : "";
            std::ranges::transform(
                semantic_name,
                semantic_name.begin(),
                [](unsigned char value)
                { return static_cast<char>(value >= 'a' && value <= 'z' ? value - 'a' + 'A' : value); }
            );
            const bool is_color = semantic_name.starts_with("SV_TARGET");
            if (semantic_name.starts_with("SV_") && !is_color)
            {
                return true;
            }
            if (type->getKind() == Kind::None)
            {
                return true;
            }
            const bool is_numeric = type->getKind() == Kind::Scalar || type->getKind() == Kind::Vector;
            const auto location = is_color ? variable.getSemanticIndex() : variable.getOffset(category);
            if (!is_numeric || base > UINT32_MAX || location > UINT32_MAX - base)
            {
                return false;
            }
            rdesc::EDataScalar data_scalar{};
            if (!scalar(*type, data_scalar))
            {
                return false;
            }
            rdesc::ShaderIoVariable io;
            io.name = variable.getName() ? variable.getName() : "result";
            io.location = static_cast<std::uint32_t>(base + location);
            io.semantic = semantic_name;
            io.semantic_index = static_cast<std::uint32_t>(variable.getSemanticIndex());
            io.vec_size = std::max(type->getColumnCount(), 1U);
            io.base = data_scalar == rdesc::EDataScalar::FLOAT
                          ? rdesc::EVertexScalarBase::FLOAT
                          : (data_scalar == rdesc::EDataScalar::SINT ? rdesc::EVertexScalarBase::INT
                                                                     : rdesc::EVertexScalarBase::UINT);
            result.push_back(std::move(io));
            return true;
        }

        using SessionResult = cxx::expected<Slang::ComPtr<slang::ISession>, ShaderCompileFailure>;

        SessionResult createSession(slang::IGlobalSession& global, const ShaderCompileRequest& request)
        {
            slang::TargetDesc target;
            slang::CompilerOptionEntry preserve_entry{
                slang::CompilerOptionName::VulkanUseEntryPointName,
                {slang::CompilerOptionValueKind::Int, 1}
            };
            target.compilerOptionEntries = &preserve_entry;
            target.compilerOptionEntryCount = 1;
            target.format = request.target == rdesc::EShaderTarget::SPIRV ? SLANG_SPIRV : SLANG_DXIL;
            const std::string profile(request.profile);
            target.profile = global.findProfile(profile.c_str());
            if (target.profile == SLANG_PROFILE_UNKNOWN)
            {
                return failure(EShaderCompileError::INVALID_INPUT, "unknown shader profile");
            }
            slang::SessionDesc description;
            std::vector<std::string> include_paths;
            std::vector<const char*> include_views;
            include_paths.reserve(request.include_directories.size());
            for (auto path : request.include_directories)
            {
                include_paths.emplace_back(path);
            }
            for (const auto& path : include_paths)
            {
                include_views.push_back(path.c_str());
            }
            description.searchPaths = include_views.data();
            description.searchPathCount = static_cast<SlangInt>(include_views.size());
            description.targets = &target;
            description.targetCount = 1;
            description.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
            Slang::ComPtr<slang::ISession> session;
            if (SLANG_FAILED(global.createSession(description, session.writeRef())))
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, "cannot create target session");
            }
            return session;
        }

        void recordDiagnostics(const ShaderCompileRequest& request, slang::IBlob* blob)
        {
            if (request.report && blob)
            {
                request.report->diagnostic += diagnostics(blob, "");
            }
        }

        ShaderCompileResult
        compileSource(slang::ISession& session, const ShaderCompileRequest& request, std::size_t ordinal)
        {
            const bool is_invalid_text = request.source.empty() || request.entry.empty() ||
                                         request.source.find('\0') != std::string_view::npos ||
                                         request.entry.find('\0') != std::string_view::npos;
            const bool is_supported_stage = request.stage == rdesc::EShaderType::VERTEX ||
                                            request.stage == rdesc::EShaderType::FRAGMENT ||
                                            request.stage == rdesc::EShaderType::COMPUTE;
            if (is_invalid_text || !is_supported_stage || request.target > rdesc::EShaderTarget::DXIL)
            {
                return failure(EShaderCompileError::INVALID_INPUT, "invalid shader source, stage or target");
            }
            Slang::ComPtr<slang::IBlob> diagnostic;
            const std::string source = std::string(
                                           request.target == rdesc::EShaderTarget::DXIL ? "#define LUX_TARGET_DXIL 1\n"
                                                                                        : "#define LUX_TARGET_SPIRV 1\n"
                                       ) +
                                       std::string(request.source);
            const std::string entry_name(request.entry), source_path(request.source_path);
            // Content identity plus a batch-local ordinal: hashes never authorize cache equality.
            std::uint64_t fingerprint = 1469598103934665603ULL;
            for (unsigned char byte : source + source_path)
            {
                fingerprint = (fingerprint ^ byte) * 1099511628211ULL;
            }
            const auto module_name = "lux_shader_" + std::to_string(fingerprint) + "_" + std::to_string(ordinal);
            const auto module_path =
                (std::filesystem::path(source_path).parent_path() / (module_name + ".slang")).string();
            auto* module = session.loadModuleFromSourceString(
                module_name.c_str(),
                module_path.c_str(),
                source.c_str(),
                diagnostic.writeRef()
            );
            recordDiagnostics(request, diagnostic);
            if (!module)
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, diagnostics(diagnostic, "cannot load source"));
            }
            Slang::ComPtr<slang::IEntryPoint> entry;
            if (SLANG_FAILED(module->findEntryPointByName(entry_name.c_str(), entry.writeRef())))
            {
                return failure(EShaderCompileError::INVALID_INPUT, "entry point not found");
            }
            slang::IComponentType* components[]{module, entry};
            Slang::ComPtr<slang::IComponentType> composite, linked;
            auto status =
                session.createCompositeComponentType(components, 2, composite.writeRef(), diagnostic.writeRef());
            recordDiagnostics(request, diagnostic);
            if (SLANG_FAILED(status))
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, diagnostics(diagnostic, "cannot compose shader"));
            }
            status = composite->link(linked.writeRef(), diagnostic.writeRef());
            recordDiagnostics(request, diagnostic);
            if (SLANG_FAILED(status))
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, diagnostics(diagnostic, "cannot link shader"));
            }
            Slang::ComPtr<slang::IBlob> code;
            status = linked->getEntryPointCode(0, 0, code.writeRef(), diagnostic.writeRef());
            recordDiagnostics(request, diagnostic);
            if (SLANG_FAILED(status))
            {
                return
                failure(EShaderCompileError::COMPILER_FAILURE, diagnostics(diagnostic, "target generation failed"));
            }
            auto* layout = linked->getLayout(0, diagnostic.writeRef());
            recordDiagnostics(request, diagnostic);
            if (!layout || layout->getEntryPointCount() != 1 || code->getBufferSize() % 4 != 0)
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, "invalid target output");
            }
            const auto expected_stage =
                request.stage == rdesc::EShaderType::VERTEX
                    ? SLANG_STAGE_VERTEX
                    : (request.stage == rdesc::EShaderType::FRAGMENT ? SLANG_STAGE_FRAGMENT : SLANG_STAGE_COMPUTE);
            auto* reflected_entry = layout->getEntryPointByIndex(0);
            if (reflected_entry->getStage() != expected_stage)
            {
                return failure(EShaderCompileError::INVALID_INPUT, "entry stage mismatch");
            }
            rdesc::ShaderProgramArtifact result;
            result.target = request.target;
            auto& binary = result.stages.emplace_back();
            binary.stage = request.stage;
            binary.entry = entry_name;
            binary.words.resize(code->getBufferSize() / 4);
            std::memcpy(binary.words.data(), code->getBufferPointer(), code->getBufferSize());
            if (!rdesc::validShaderContainer(result.target, binary.words))
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, "invalid target container");
            }
            const auto stages = request.stage == rdesc::EShaderType::VERTEX
                                    ? 1U
                                    : (request.stage == rdesc::EShaderType::FRAGMENT ? 2U : 4U);
            for (unsigned index = 0; index < layout->getParameterCount(); ++index)
            {
                if (!resource(*layout->getParameterByIndex(index), stages, result.interface))
                {
                    return
                    failure(
                        EShaderCompileError::UNSUPPORTED_LAYOUT,
                        std::string("unsupported resource layout or register collision: ") +
                            layout->getParameterByIndex(index)->getName() + " kind=" +
                            std::to_string(
                                static_cast<unsigned>(layout->getParameterByIndex(index)->getTypeLayout()->getKind())
                            ) +
                            " category=" +
                            std::to_string(static_cast<unsigned>(layout->getParameterByIndex(index)->getCategory()))
                    );
                }
            }
            if (request.report)
            {
                Slang::ComPtr<slang::IMetadata> metadata;
                const auto metadata_status =
                    linked->getEntryPointMetadata(0, 0, metadata.writeRef(), diagnostic.writeRef());
                recordDiagnostics(request, diagnostic);
                for (unsigned index = 0; index < layout->getParameterCount(); ++index)
                {
                    auto* parameter = layout->getParameterByIndex(index);
                    ShaderParameterUse usage{
                        parameter->getName(),
                        parameter->getBindingSpace(),
                        parameter->getBindingIndex()
                    };
                    bool used = false;
                    const bool has_usage = SLANG_SUCCEEDED(metadata_status) && metadata &&
                                           SLANG_SUCCEEDED(metadata->isParameterLocationUsed(
                                               static_cast<SlangParameterCategory>(parameter->getCategory()),
                                               usage.space,
                                               usage.binding,
                                               used
                                           ));
                    if (has_usage)
                    {
                        usage.use = used ? EShaderParameterUse::USED : EShaderParameterUse::UNUSED;
                    }
                    request.report->parameters.push_back(std::move(usage));
                }
            }
            rdesc::EntryPointInfo entry_info{entry_name, request.stage};
            for (unsigned index = 0; index < reflected_entry->getParameterCount(); ++index)
            {
                if (!stageIo(*reflected_entry->getParameterByIndex(index), entry_info.inputs))
                {
                    return failure(EShaderCompileError::UNSUPPORTED_LAYOUT, "unsupported stage input");
                }
            }
            if (auto* output = reflected_entry->getResultVarLayout(); output && !stageIo(*output, entry_info.outputs))
            {
                return failure(EShaderCompileError::UNSUPPORTED_LAYOUT, "unsupported stage output");
            }
            if (request.stage == rdesc::EShaderType::VERTEX)
            {
                result.interface.vertex_inputs = entry_info.inputs;
            }
            result.interface.entry_points.push_back(std::move(entry_info));
            // Use the same canonical set order as the asset codec, independent of declaration order.
            std::ranges::sort(result.interface.sets, {}, &rdesc::DescriptorSetLayoutInfo::set);
            return result;
        }
    } // namespace

    struct ShaderCompiler::Impl final
    {
        Slang::ComPtr<slang::IGlobalSession> global;
        std::filesystem::path runtime;
    };

    ShaderCompiler::ShaderCompiler(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    ShaderCompiler::~ShaderCompiler() noexcept = default;

    ShaderCompiler::CreateResult ShaderCompiler::create() noexcept
    {
        try
        {
            auto impl = std::make_unique<Impl>();
            if (SLANG_FAILED(slang::createGlobalSession(impl->global.writeRef())))
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, "cannot create Slang global session");
            }
            impl->runtime = runtimeDirectory();
            if (impl->runtime.empty())
            {
                return failure(EShaderCompileError::COMPILER_FAILURE, "cannot locate compiler runtime");
            }
            impl->global->setDownstreamCompilerPath(SLANG_PASS_THROUGH_DXC, impl->runtime.string().c_str());
            return std::unique_ptr<ShaderCompiler>(new ShaderCompiler(std::move(impl)));
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& exception)
        {
            return failure(EShaderCompileError::COMPILER_FAILURE, exception.what());
        }
        catch (...)
        {
            return failure(EShaderCompileError::COMPILER_FAILURE, "foreign shader compiler initialization failure");
        }
    }

    ShaderCompileBatchResult ShaderCompiler::compileBatch(std::span<const ShaderCompileRequest> requests) noexcept
    {
        try
        {
            std::vector<rdesc::ShaderProgramArtifact> result;
            result.reserve(requests.size());
            Slang::ComPtr<slang::ISession> session;
            const ShaderCompileRequest* previous = nullptr;
            for (const auto& request : requests)
            {
                if (request.report)
                {
                    *request.report = {};
                }
                const bool has_invalid_option =
                    request.profile.find('\0') != std::string_view::npos ||
                    request.source_path.find('\0') != std::string_view::npos ||
                    std::ranges::any_of(
                        request.include_directories,
                        [](std::string_view path) { return path.find('\0') != std::string_view::npos; }
                    );
                if (has_invalid_option)
                {
                    return failure(EShaderCompileError::INVALID_INPUT, "embedded null in shader options");
                }
#ifdef _WIN32
                const auto dxc = impl_->runtime / "dxcompiler.dll";
#else
                const auto dxc = impl_->runtime / "libdxcompiler.so";
#endif
                if (request.target == rdesc::EShaderTarget::DXIL && !std::filesystem::is_regular_file(dxc))
                {
                    return failure(EShaderCompileError::COMPILER_FAILURE, "pinned DXC runtime is missing");
                }
                const bool same_options =
                    previous && previous->target == request.target && previous->profile == request.profile &&
                    std::ranges::equal(previous->include_directories, request.include_directories);
                if (!same_options)
                {
                    auto created = createSession(*impl_->global, request);
                    if (!created)
                    {
                        return cxx::unexpected(std::move(created.error()));
                    }
                    session = std::move(*created);
                }
                auto compiled = compileSource(*session, request, result.size());
                if (!compiled)
                {
                    return cxx::unexpected(std::move(compiled.error()));
                }
                result.push_back(std::move(*compiled));
                previous = &request;
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& exception)
        {
            return failure(EShaderCompileError::COMPILER_FAILURE, exception.what());
        }
        catch (...)
        {
            return failure(EShaderCompileError::COMPILER_FAILURE, "foreign shader compiler failure");
        }
    }

    ShaderCompileResult ShaderCompiler::compile(const ShaderCompileRequest& request) noexcept
    {
        auto result = compileBatch({&request, 1});
        if (!result)
        {
            return cxx::unexpected(std::move(result.error()));
        }
        return std::move(result->front());
    }

    std::string_view compilerVersion() noexcept
    {
        return LUX_SHADER_COMPILER_VERSION;
    }
} // namespace lux::shader
