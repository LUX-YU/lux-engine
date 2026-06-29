// packer.cpp
#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MeshSerDeser.hpp>
// #include <lux/engine/asset/ModelSerDeser.hpp> // 不需要
#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/asset/ShaderSerDeser.hpp>

#include <lux/engine/asset/AssetSerDeser.hpp>   // 为了 AssetFileHeader / loadHeader
#include <lux/engine/asset/PakCook.hpp>         // pack / pak-inspect 模式
#include <lux/cxx/arguments/Arguments.hpp>

#include <iostream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>
#include <algorithm>
#include <optional>
#include <cctype>

namespace fs = std::filesystem;

// ========== 本工具用到的一些小工具 ==========
static const char* to_string(lux::asset::EShaderType t) {
    using lux::asset::EShaderType;
    switch (t) {
    case EShaderType::VERTEX:   return "VERTEX";
    case EShaderType::FRAGMENT: return "FRAGMENT";
    case EShaderType::GEOMETRY: return "GEOMETRY";
    case EShaderType::COMPUTE:  return "COMPUTE";
    default: return "UNKNOWN";
    }
}

static std::string toError(lux::asset::EAssetError e)
{
    using lux::asset::EAssetError;
    switch (e) {
    case EAssetError::SUCCESS:                  return "SUCCESS";
    case EAssetError::RELATED_ASSET_LOAD_ERROR: return "RELATED_ASSET_LOAD_ERROR";
    case EAssetError::FILE_NOT_EXIST:           return "FILE_NOT_EXIST";
    case EAssetError::FILE_TYPE_ERROR:          return "FILE_TYPE_ERROR";
    case EAssetError::FILE_OPEN_FAIL:           return "FILE_OPEN_FAIL";
    case EAssetError::WRITE_FILE_FAIL:          return "WRITE_FILE_FAIL";
    case EAssetError::READ_FILE_FAIL:           return "READ_FILE_FAIL";
    case EAssetError::OUT_OF_MEMORY:            return "OUT_OF_MEMORY";
    case EAssetError::TARGET_NOT_EXIST:         return "TARGET_NOT_EXIST";
    case EAssetError::TARGET_IS_NOT_DIRECTORY:  return "TARGET_IS_NOT_DIRECTORY";
    case EAssetError::TARGET_IS_NOT_FILE:       return "TARGET_IS_NOT_FILE";
    case EAssetError::RELEASED:                 return "RELEASED";
    case EAssetError::UNSUPPORTED:              return "UNSUPPORTED";
    case EAssetError::ABNORMAL_FILE_SIZE:       return "ABNORMAL_FILE_SIZE";
    case EAssetError::WRONG_FILE_HEADER:        return "WRONG_FILE_HEADER";
    case EAssetError::ASSET_NO_DATA:            return "ASSET_NO_DATA";
    case EAssetError::ASSET_ALREADY_EXIST:      return "ASSET_ALREADY_EXIST";
    case EAssetError::ASSET_NOT_EXIST:          return "ASSET_NOT_EXIST";
    case EAssetError::ASSET_NO_INFO:            return "ASSET_NO_INFO";
    case EAssetError::UNSUPPORTED_VERSION:      return "UNSUPPORTED_VERSION";
    case EAssetError::UNKNOWN_FILESYSTEM_ERROR: return "UNKNOWN_FILESYSTEM_ERROR";
    case EAssetError::UNKNOWN_ERROR:            return "UNKNOWN_ERROR";
    default:                                     return "INVALID_ERROR_CODE";
    }
}

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static std::optional<lux::rdesc::ETexturePixelFormat>
parseTexturePixelFormat(const std::string& raw)
{
    using lux::rdesc::ETexturePixelFormat;
    const std::string s = toLower(raw);

    if (s == "rgba8_unorm") return ETexturePixelFormat::RGBA8_UNORM;
    if (s == "rgba8_srgb") return ETexturePixelFormat::RGBA8_SRGB;
    if (s == "rg8_unorm") return ETexturePixelFormat::RG8_UNORM;
    if (s == "r8_unorm") return ETexturePixelFormat::R8_UNORM;
    if (s == "rgba16_sfloat") return ETexturePixelFormat::RGBA16_SFLOAT;
    if (s == "bc1_srgb") return ETexturePixelFormat::BC1_SRGB;
    if (s == "bc3_srgb") return ETexturePixelFormat::BC3_SRGB;
    if (s == "bc5_unorm") return ETexturePixelFormat::BC5_UNORM;
    if (s == "bc7_srgb") return ETexturePixelFormat::BC7_SRGB;
    if (s == "etc2_rgb8_unorm") return ETexturePixelFormat::ETC2_RGB8_UNORM;
    if (s == "etc2_rgb8_srgb") return ETexturePixelFormat::ETC2_RGB8_SRGB;
    if (s == "etc2_rgba8_unorm") return ETexturePixelFormat::ETC2_RGBA8_UNORM;
    if (s == "etc2_rgba8_srgb") return ETexturePixelFormat::ETC2_RGBA8_SRGB;
    if (s == "astc_4x4_unorm") return ETexturePixelFormat::ASTC_4x4_UNORM;
    if (s == "astc_4x4_srgb") return ETexturePixelFormat::ASTC_4x4_SRGB;
    if (s == "astc_6x6_unorm") return ETexturePixelFormat::ASTC_6x6_UNORM;
    if (s == "astc_6x6_srgb") return ETexturePixelFormat::ASTC_6x6_SRGB;

    return std::nullopt;
}

static std::optional<lux::rdesc::ETextureColorSpace>
parseTextureColorSpace(const std::string& raw)
{
    using lux::rdesc::ETextureColorSpace;
    const std::string s = toLower(raw);
    if (s == "srgb") return ETextureColorSpace::SRGB;
    if (s == "linear") return ETextureColorSpace::LINEAR;
    if (s == "data") return ETextureColorSpace::DATA;
    if (s == "unknown") return ETextureColorSpace::UNKNOWN;
    return std::nullopt;
}

static bool supportsCompressedEncodingInPacker(lux::rdesc::ETexturePixelFormat fmt)
{
    using lux::rdesc::ETexturePixelFormat;
    switch (fmt)
    {
    case ETexturePixelFormat::BC1_SRGB:
    case ETexturePixelFormat::BC3_SRGB:
    case ETexturePixelFormat::BC5_UNORM:
    case ETexturePixelFormat::BC7_SRGB:
        return true;
    default:
        return false;
    }
}

// ========== CLI 参数解析结构 ==========
struct UsedArguments
{
    lux::asset::EAssetType type;
    fs::path  source_path;
    fs::path  target_path;
};

// ========== 类型解析/工厂 ==========
static lux::asset::EAssetType stringToAssetType(const std::string& type_str)
{
    if (type_str == "mesh")     return lux::asset::EAssetType::MESH;
    if (type_str == "texture")  return lux::asset::EAssetType::TEXTURE;
    if (type_str == "shader")   return lux::asset::EAssetType::SHADER;
    return lux::asset::EAssetType::UNKNOWN;
}

static std::shared_ptr<lux::asset::AssetSerDeser> makeSerDeser(
    std::shared_ptr<lux::asset::AssetManager> manager, lux::asset::EAssetType type)
{
    // Type->SerDeser table lives in the asset module (covers every serialized
    // type); which types this tool ACCEPTS is still gated by parseType().
    return lux::asset::createSerDeserFor(type, std::move(manager));
}

// ========== 校验导入模式参数 ==========
static bool argument_check_import(const lux::cxx::ParsedOptions& options, UsedArguments& arguments)
{
    auto type_str = options.get("type").as<std::string>();
    auto source_path_str = options.get("source_path").as<std::string>();
    auto target_path_str = options.get("target_path").as<std::string>();

    if (!type_str || !source_path_str || !target_path_str) {
        std::cerr << "Missing required options for import mode.\n";
        return false;
    }

    // 资产类型
    arguments.type = stringToAssetType(type_str.value());
    if (arguments.type == lux::asset::EAssetType::UNKNOWN) {
        std::cerr << "Unknown asset type: " << type_str.value() << std::endl;
        return false;
    }

    // 源文件存在
    arguments.source_path = source_path_str.value();
    if (!fs::exists(arguments.source_path) || !fs::is_regular_file(arguments.source_path)) {
        std::cerr << "source file does not exist or not a file: " << arguments.source_path << std::endl;
        return false;
    }

    // 目标路径
    arguments.target_path = target_path_str.value();
    // 这里不强制文件不存在；父目录不存在时可以交给底层导出创建
    return true;
}

// ========== inspect 模式实现 ==========
static void printAssetInfo(const lux::asset::AssetInfo& ai) {
    std::cout << "  AssetInfo:\n";
    // 若 asset_id_t 没有 << 重载，可自行格式化为 hex 字节串
    std::cout << "    id   : " << ai.id << "\n";
    std::cout << "    type : " << static_cast<int>(ai.type) << "\n";
    std::cout << "    date : " << ai.date << "\n";
}

static int inspectLuxAsset(const fs::path& file_path)
{
    using namespace lux::asset;

    // 读取整个文件到内存
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        std::cerr << "Failed to open: " << file_path << "\n";
        return -1;
    }
    ifs.seekg(0, std::ios::end);
    const auto n = ifs.tellg();
    if (n <= 0) {
        std::cerr << "Abnormal file size.\n";
        return -1;
    }
    ifs.seekg(0, std::ios::beg);

    std::vector<std::byte> buf(static_cast<size_t>(n));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
        std::cerr << "Read file fail.\n";
        return -1;
    }

    if (buf.size() < sizeof(AssetFileHeader)) {
        std::cerr << "Too small for AssetFileHeader.\n";
        return -1;
    }

    AssetFileHeader header{};
    std::memcpy(&header, buf.data(), sizeof(AssetFileHeader));

    auto print_common = [&]() {
        std::cout << "File      : " << file_path << "\n";
        std::cout << "Version   : " << header.version << "\n";
        std::cout << "Offsets   : info@" << header.info_offset
            << " (size=" << header.info_size << "), data@"
            << header.data_offset << " (size=" << header.data_size << ")\n";
        printAssetInfo(header.info);
        };

    // 小工具：用基类模板 loadHeader 做严格校验并打印特定信息
    auto check_and_print = [&](auto tag_type)->int {
        using TTag = decltype(tag_type);
        constexpr EAssetType K = TTag::value;

        if constexpr (K == EAssetType::SHADER)
        {
            // 1) 只解析头（原来的 InfoType 不再是标准布局）
            AssetFileHeader full_header{};
            if (auto ec = AssetSerDeser::loadHeaderRaw<K>(buf, full_header);
                ec != EAssetError::SUCCESS) {
                std::cerr << "Header validation failed: " << toError(ec) << "\n";
                return -1;
            }

            // 打印公共信息
            std::memcpy(&header, &full_header, sizeof(header));
            print_common();

            // 2) 反序列化 ShaderInfo（指令流协议）
            if (buf.size() < full_header.info_offset + full_header.info_size) {
                std::cerr << "Info region out of range.\n";
                return -1;
            }
            std::vector<std::byte> info_bytes(full_header.info_size);
            std::memcpy(info_bytes.data(), buf.data() + full_header.info_offset, full_header.info_size);

            ShaderInfo sinfo{};
            std::string err;
            if (!ShaderInfo::deserialize(info_bytes, sinfo, &err)) {
                std::cerr << "ShaderInfo deserialize failed: " << err << "\n";
                return -1;
            }

            // 3) 打印 ShaderInfo 概览
            std::cout << "ShaderInfo:\n";
            // entry points
            std::cout << "  entry_points: " << sinfo.entry_points.size() << "\n";
            for (const auto& ep : sinfo.entry_points) {
                std::cout << "    - name=" << ep.name << ", stage=" << lux::asset::to_string(ep.stage) << "\n";
            }

            // descriptor sets
            std::cout << "  descriptor_sets: " << sinfo.sets.size() << "\n";
            for (const auto& set : sinfo.sets) {
                std::cout << "    set[" << set.set << "]: " << set.bindings.size() << " bindings\n";
                for (const auto& b : set.bindings) {
                    std::cout << "      binding=" << b.binding
                        << ", type=" << to_string(b.type)
                        << ", count=" << b.count
                        << ", name=\"" << b.name << "\"";
                    if (b.type == EDescriptorType::UNIFORM_BUFFER || b.type == EDescriptorType::STORAGE_BUFFER)
                        std::cout << ", blockSize=" << b.blockSize;
                    if (b.type == EDescriptorType::STORAGE_BUFFER || b.type == EDescriptorType::STORAGE_IMAGE)
                        std::cout << ", writable=" << (b.writable ? "true" : "false");
                    std::cout << "\n";
                }
            }

            // push constants
            std::cout << "  push_constants: " << sinfo.push_constants.size() << "\n";
            for (const auto& pc : sinfo.push_constants) {
                std::cout << "    - offset=" << pc.offset << ", size=" << pc.size << "\n";
            }

            // spec constants
            std::cout << "  spec_constants: " << sinfo.spec_constants.size() << "\n";
            for (const auto& sc : sinfo.spec_constants) {
                std::cout << "    - id=" << sc.id
                    << ", constID=" << sc.constant_id
                    << ", name=\"" << sc.name << "\""
                    << ", kind=" << int(sc.default_value.kind)
                    << ", bit_width=" << sc.default_value.bit_width
                    << ", vec=" << sc.vec_size
                    << ", cols=" << sc.columns
                    << "\n";
            }

            // vertex inputs
            std::cout << "  vertex_inputs: " << sinfo.vertex_inputs.size() << "\n";
            for (const auto& vi : sinfo.vertex_inputs) {
                std::cout << "    - loc=" << vi.location
                    << ", name=\"" << vi.name << "\""
                    << ", base=" << int(vi.base)
                    << ", vec=" << vi.vec_size
                    << ", cols=" << vi.columns
                    << ", array=" << vi.array_size
                    << "\n";
            }

            // 4) payload 范围复核 + SPIR-V 魔数
            if (buf.size() < full_header.data_offset + full_header.data_size) {
                std::cerr << "Payload out of range.\n";
                return -1;
            }
            std::cout << "Payload   : " << full_header.data_size << " bytes\n";
            if (full_header.data_size >= 4) {
                uint32_t magic_spv = 0;
                std::memcpy(&magic_spv, buf.data() + full_header.data_offset, 4);
                if (magic_spv == 0x07230203)
                    std::cout << "  note    : SPIR-V magic OK\n";
                else
                    std::cout << "  note    : SPIR-V magic mismatch (0x"
                    << std::hex << magic_spv << std::dec << ")\n";
            }
            return 0;
        }
        else
        {
            // 原路径：标准布局 InfoType
            using InfoType =
                std::conditional_t<K == EAssetType::MESH, MeshAssetInfo,
                std::conditional_t<K == EAssetType::TEXTURE, TextureAssetInfo,
                void>>;

            static_assert(!std::is_void_v<InfoType>, "InfoType mapping missing for this asset type.");
            static_assert(std::is_standard_layout_v<InfoType>, "InfoType must be standard layout.");

            AssetFileHeader full_header{};
            InfoType info{};
            EAssetError ec = AssetSerDeser::loadHeader<InfoType, K>(buf, full_header, info);
            if (ec != EAssetError::SUCCESS) {
                std::cerr << "Header validation failed: " << toError(ec) << "\n";
                return -1;
            }

            std::memcpy(&header, &full_header, sizeof(header));
            print_common();

            if constexpr (K == EAssetType::MESH) {
                std::cout << "MeshInfo:\n";
                std::cout << "  (fields...)\n";
            }
            else if constexpr (K == EAssetType::TEXTURE) {
                std::cout << "TextureInfo:\n";
                std::cout << "  width        : " << info.width << "\n";
                std::cout << "  height       : " << info.height << "\n";
                std::cout << "  channels     : " << info.channel << "\n";
                std::cout << "  layers       : " << info.layers << "\n";
                std::cout << "  mip_count    : " << info.mip_count << "\n";
                std::cout << "  pixel_format : " << info.pixel_format << "\n";
                std::cout << "  color_space  : " << info.color_space << "\n";
                std::cout << "  flags        : 0x" << std::hex << info.flags << std::dec << "\n";

                const uint32_t mip_count = std::min<uint32_t>(
                    info.mip_count,
                    lux::rdesc::kTextureMaxMipCount);
                for (uint32_t i = 0; i < mip_count; ++i)
                {
                    const auto& mip = info.mip_ranges[i];
                    std::cout << "    mip[" << i << "]:"
                              << " offset=" << mip.offset
                              << " size=" << mip.size
                              << " extent=" << mip.width << "x" << mip.height
                              << "\n";
                }
            }

            if (buf.size() < full_header.data_offset + full_header.data_size) {
                std::cerr << "Payload out of range.\n";
                return -1;
            }
            std::cout << "Payload   : " << full_header.data_size << " bytes\n";
            return 0;
        }
        };

    const auto magic = header.magic_number;
    if (magic == asset_magic_number_of<EAssetType::MESH>::value)
        return check_and_print(std::integral_constant<EAssetType, EAssetType::MESH>{});
    if (magic == asset_magic_number_of<EAssetType::TEXTURE>::value)
        return check_and_print(std::integral_constant<EAssetType, EAssetType::TEXTURE>{});
    if (magic == asset_magic_number_of<EAssetType::SHADER>::value)
        return check_and_print(std::integral_constant<EAssetType, EAssetType::SHADER>{});

    std::cerr << "Unknown magic number: 0x" << std::hex << magic << std::dec << "\n";
    return -1;
}

// ========== embed 模式实现 ==========
static int embedLuxAsset(const fs::path& luxasset_path,
                          const fs::path& output_path,
                          const std::string& embed_name,
                          const std::string& embed_ns)
{
    using namespace lux::asset;

    // 1. Read .luxasset file
    std::ifstream ifs(luxasset_path, std::ios::binary);
    if (!ifs) {
        std::cerr << "Failed to open: " << luxasset_path << "\n";
        return -1;
    }
    ifs.seekg(0, std::ios::end);
    const auto n = ifs.tellg();
    if (n <= 0) {
        std::cerr << "Abnormal file size.\n";
        return -1;
    }
    ifs.seekg(0, std::ios::beg);

    std::vector<std::byte> buf(static_cast<size_t>(n));
    if (!ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
        std::cerr << "Read file fail.\n";
        return -1;
    }
    ifs.close();

    // 2. Parse header
    AssetFileHeader header{};
    if (auto ec = AssetSerDeser::loadHeaderRaw<EAssetType::SHADER>(buf, header);
        ec != EAssetError::SUCCESS) {
        std::cerr << "Header validation failed: " << toError(ec) << "\n";
        return -1;
    }

    // 3. Validate regions
    if (buf.size() < header.info_offset + header.info_size ||
        buf.size() < header.data_offset + header.data_size) {
        std::cerr << "File truncated.\n";
        return -1;
    }

    const std::byte* info_begin = buf.data() + header.info_offset;
    const size_t info_size = static_cast<size_t>(header.info_size);
    const std::byte* data_begin = buf.data() + header.data_offset;
    const size_t data_size = static_cast<size_t>(header.data_size);

    // 4. Verify SPIR-V magic
    if (data_size >= 4) {
        uint32_t magic_spv = 0;
        std::memcpy(&magic_spv, data_begin, 4);
        if (magic_spv != 0x07230203) {
            std::cerr << "Warning: SPIR-V magic mismatch (0x"
                      << std::hex << magic_spv << std::dec << ")\n";
        }
    }

    // 5. Create parent directory if needed
    if (auto parent = output_path.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    // 6. Generate C++ header
    std::ofstream ofs(output_path, std::ios::trunc);
    if (!ofs) {
        std::cerr << "Failed to create output file: " << output_path << "\n";
        return -1;
    }

    ofs << "// Auto-generated by lux_asset_packer --embed. Do not edit.\n";
    ofs << "#pragma once\n";
    ofs << "#include <cstddef>\n";
    ofs << "#include <cstdint>\n\n";

    if (!embed_ns.empty()) {
        ofs << "namespace " << embed_ns << " {\n\n";
    }

    auto write_array = [&](const char* suffix, const std::byte* p, size_t sz) {
        ofs << "alignas(4) inline constexpr uint8_t " << embed_name << suffix << "[] = {";
        for (size_t i = 0; i < sz; ++i) {
            if (i % 16 == 0) ofs << "\n    ";
            ofs << "0x" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned>(p[i]) << std::dec;
            if (i + 1 < sz) ofs << ",";
        }
        ofs << "\n};\n";
        ofs << "inline constexpr std::size_t " << embed_name << suffix << "_size = "
            << sz << ";\n\n";
    };

    write_array("_spirv", data_begin, data_size);
    write_array("_info", info_begin, info_size);

    if (!embed_ns.empty()) {
        ofs << "} // namespace " << embed_ns << "\n";
    }

    std::cout << "Embedded: " << output_path
              << " (spirv=" << data_size << " info=" << info_size << " bytes)\n";
    return 0;
}

// ========== 主程序 ==========
int main(int argc, char* argv[])
{
    using namespace lux::asset;

    lux::cxx::Parser parser;
    parser.add<std::string>("type", "t")
        .desc("Type of asset to process (mesh, texture, shader)")
        .required(false); // 在 inspect 模式下不需要

    parser.add<std::string>("source_path", "sp")
        .desc("Path to the source file")
        .required(false); // 在 inspect 模式下不需要

    parser.add<std::string>("target_path", "tp")
        .desc("Path to the asset file (.luxasset) or output path when packing")
        .required();

    parser.add<bool>("inspect", "i")
        .desc("Inspect an existing .luxasset file (read-only). When set, only --target_path is required.")
        .def(false);

    parser.add<std::string>("texture_format", "tf")
        .desc("Texture output format metadata (e.g. rgba8_srgb, bc7_srgb, astc_6x6_srgb)")
        .required(false)
        .def("rgba8_srgb");

    parser.add<std::string>("texture_color_space", "tcs")
        .desc("Texture color space metadata (srgb, linear, data, unknown)")
        .required(false)
        .def("srgb");

    parser.add<bool>("embed", "e")
        .desc("Embed mode: read a .luxasset shader and generate a C++ header with constexpr byte arrays. "
              "Uses --target_path as input .luxasset, --source_path as output .hpp.")
        .def(false);

    parser.add<bool>("pack", "pk")
        .desc("Pack mode: cook a loose content folder into a .luxpak. "
              "Uses --source_path as the content dir, --target_path as the output .luxpak.")
        .def(false);

    parser.add<bool>("pak_inspect", "pi")
        .desc("Pak-inspect mode: print a .luxpak's index (read-only). "
              "Uses --target_path as the pak file.")
        .def(false);

    parser.add<std::string>("mount_hint", "mh")
        .desc("Advisory mount root recorded in the pak when packing (default /Game)")
        .required(false)
        .def("/Game");

    parser.add<std::string>("embed_name", "en")
        .desc("C++ identifier prefix for the embedded arrays (e.g. grid_vert)")
        .required(false);

    parser.add<std::string>("embed_namespace", "ens")
        .desc("C++ namespace for the generated header (e.g. lux::render::builtin)")
        .required(false)
        .def("lux::render::builtin");

    auto expected_parse_rst = parser.parse(argc, argv);
    if (!expected_parse_rst) {
        std::cerr << parser.usage() << std::endl;
        return -1;
    }
    const auto& opts = expected_parse_rst.value();

    const bool inspect_mode = opts.get("inspect").as<bool>().value();
    if (inspect_mode) {
        // 仅使用 target_path
        auto target_path_str = opts.get("target_path").as<std::string>();
        if (!target_path_str) {
            std::cerr << "In --inspect mode, --target_path is required.\n";
            return -1;
        }
        fs::path target_path = target_path_str.value();
        if (!fs::exists(target_path) || !fs::is_regular_file(target_path)) {
            std::cerr << "Target file does not exist or not a file: " << target_path << "\n";
            return -1;
        }
        return inspectLuxAsset(target_path);
    }

    const bool pack_mode = opts.get("pack").as<bool>().value();
    if (pack_mode) {
        auto source_path_str = opts.get("source_path").as<std::string>();
        auto target_path_str = opts.get("target_path").as<std::string>();
        if (!source_path_str || !target_path_str) {
            std::cerr << "In --pack mode, --source_path (content dir) and "
                         "--target_path (output .luxpak) are required.\n";
            return -1;
        }
        const auto mount_hint =
            opts.get("mount_hint").as<std::string>().value_or("/Game");

        auto cooked = lux::asset::cookDirectoryToPak(
            fs::path(source_path_str.value()),
            fs::path(target_path_str.value()),
            mount_hint
        );
        if (!cooked) {
            std::cerr << "pack failed: " << cooked.error() << "\n";
            return -1;
        }
        std::cout << "Packed " << cooked.value().asset_count << " asset(s), "
                  << cooked.value().payload_bytes << " payload byte(s) -> "
                  << target_path_str.value() << "\n";
        return 0;
    }

    const bool pak_inspect_mode = opts.get("pak_inspect").as<bool>().value();
    if (pak_inspect_mode) {
        auto target_path_str = opts.get("target_path").as<std::string>();
        if (!target_path_str) {
            std::cerr << "In --pak_inspect mode, --target_path is required.\n";
            return -1;
        }
        auto info = lux::asset::inspectPak(fs::path(target_path_str.value()));
        if (!info) {
            std::cerr << "pak-inspect failed: " << info.error() << "\n";
            return -1;
        }
        std::cout << "pak: " << target_path_str.value() << "\n"
                  << "mount hint: " << info.value().mount_hint << "\n"
                  << "entries: " << info.value().entries.size() << "\n";
        for (const auto& e : info.value().entries) {
            std::cout << "  " << uuids::to_string(e.id)
                      << "  type=" << static_cast<int>(e.type)
                      << "  off=" << e.offset
                      << "  size=" << e.size
                      << (e.tombstone ? "  TOMBSTONE" : "")
                      << "  vpath=" << (e.vpath.empty() ? "<none>" : e.vpath)
                      << "\n";
        }
        return 0;
    }

    const bool embed_mode = opts.get("embed").as<bool>().value();
    if (embed_mode) {
        auto target_path_str = opts.get("target_path").as<std::string>();
        auto source_path_str = opts.get("source_path").as<std::string>();
        auto embed_name_str  = opts.get("embed_name").as<std::string>();
        auto embed_ns_str    = opts.get("embed_namespace").as<std::string>();

        if (!target_path_str || !source_path_str || !embed_name_str) {
            std::cerr << "In --embed mode, --target_path (input .luxasset), "
                         "--source_path (output .hpp), and --embed_name are required.\n";
            return -1;
        }

        fs::path input_path = target_path_str.value();
        if (!fs::exists(input_path) || !fs::is_regular_file(input_path)) {
            std::cerr << "Input .luxasset does not exist or not a file: " << input_path << "\n";
            return -1;
        }

        return embedLuxAsset(
            input_path,
            fs::path(source_path_str.value()),
            embed_name_str.value(),
            embed_ns_str.value_or("lux::render::builtin")
        );
    }

    // ------- 打包模式（import → export） -------
    UsedArguments arguments{};
    if (!argument_check_import(opts, arguments)) {
        return -1;
    }

    auto asset_manager = std::make_shared<lux::asset::AssetManager>();
    auto serdeser = makeSerDeser(asset_manager, arguments.type);
    if (!serdeser) {
        std::cerr << "No SerDeser available for type.\n";
        return -1;
    }

    if (arguments.type == EAssetType::TEXTURE)
    {
        auto tex_serdeser = std::dynamic_pointer_cast<TextureSerDeser>(serdeser);
        if (!tex_serdeser)
        {
            std::cerr << "Failed to acquire TextureSerDeser.\n";
            return -1;
        }

        const auto tf = opts.get("texture_format").as<std::string>().value_or("rgba8_srgb");
        const auto tc = opts.get("texture_color_space").as<std::string>().value_or("srgb");

        auto fmt = parseTexturePixelFormat(tf);
        if (!fmt)
        {
            std::cerr << "Invalid --texture_format: " << tf << "\n";
            return -1;
        }
        auto cspace = parseTextureColorSpace(tc);
        if (!cspace)
        {
            std::cerr << "Invalid --texture_color_space: " << tc << "\n";
            return -1;
        }

        tex_serdeser->config().output_format = *fmt;
        tex_serdeser->config().color_space = *cspace;

        if (lux::rdesc::isCompressedFormat(*fmt) && !supportsCompressedEncodingInPacker(*fmt))
        {
            std::cerr << "Unsupported compressed --texture_format for current packer: " << tf << "\n";
            std::cerr << "Currently supported compressed formats: bc1_srgb, bc3_srgb, bc5_unorm, bc7_srgb\n";
            return -1;
        }
    }

    auto expected_import_rst = serdeser->importFromFile(arguments.source_path);
    if (!expected_import_rst) {
        std::cerr << "Failed to import asset from file: " << toError(expected_import_rst.error()) << std::endl;
        return -1;
    }

    auto& [asset, id] = expected_import_rst.value();
    if (!asset || !asset->hasData()) {
        std::cerr << "Imported asset has no data.\n";
        return -1;
    }

    auto err = serdeser->exportAsLuxAsset(id, arguments.target_path);
    if (err != EAssetError::SUCCESS) {
        std::cerr << "Failed to export asset to file: " << toError(err) << std::endl;
        return -1;
    }

    std::cout << "Packed successfully: " << arguments.target_path << std::endl;
    return 0;
}
