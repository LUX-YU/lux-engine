#include <lux/cxx/algorithm/sha256.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/storage/VirtualPath.hpp>
#include <lux/engine/resource/asset/storage/pak/PakArchive.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/toolchain/asset/shader/ShaderCooker.hpp>
#include <lux/engine/toolchain/asset/texture/TextureCooker.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    struct Options final
    {
        std::unordered_map<std::string, std::string> values;
        std::vector<std::string> flags;

        [[nodiscard]] bool has(std::string_view name) const
        {
            return std::find(flags.begin(), flags.end(), name) != flags.end();
        }

        [[nodiscard]] std::optional<std::string> value(std::string_view name) const
        {
            const auto found = values.find(std::string{name});
            if (found == values.end())
                return std::nullopt;
            return found->second;
        }
    };

    [[nodiscard]] std::optional<Options> parseOptions(int argc, char** argv)
    {
        Options result;
        for (int index = 1; index < argc; ++index)
        {
            std::string name = argv[index];
            if (!name.starts_with("--"))
                return std::nullopt;
            name.erase(0U, 2U);
            const bool flag = name == "inspect" || name == "embed" || name == "pack" ||
                name == "pak_inspect" || name == "no_mips";
            if (flag)
            {
                result.flags.push_back(std::move(name));
                continue;
            }
            if (index + 1 >= argc || std::string_view{argv[index + 1]}.starts_with("--"))
                return std::nullopt;
            result.values.emplace(std::move(name), argv[++index]);
        }
        return result;
    }

    [[nodiscard]] std::string lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    [[nodiscard]] std::optional<lux::rdesc::ETexturePixelFormat> textureFormat(std::string value)
    {
        using Format = lux::rdesc::ETexturePixelFormat;
        value = lowercase(std::move(value));
        if (value == "rgba8_unorm") return Format::RGBA8_UNORM;
        if (value == "rgba8_srgb") return Format::RGBA8_SRGB;
        if (value == "rg8_unorm") return Format::RG8_UNORM;
        if (value == "r8_unorm") return Format::R8_UNORM;
        if (value == "bc1_srgb") return Format::BC1_SRGB;
        if (value == "bc3_srgb") return Format::BC3_SRGB;
        if (value == "bc5_unorm") return Format::BC5_UNORM;
        if (value == "bc7_srgb") return Format::BC7_SRGB;
        if (value.starts_with("etc2_") || value.starts_with("astc_"))
            return Format::UNKNOWN;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<lux::rdesc::ETextureColorSpace> textureColorSpace(std::string value)
    {
        using ColorSpace = lux::rdesc::ETextureColorSpace;
        value = lowercase(std::move(value));
        if (value == "srgb") return ColorSpace::SRGB;
        if (value == "linear") return ColorSpace::LINEAR;
        if (value == "data") return ColorSpace::DATA;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<lux::cxx::SharedBytes<>> readOwned(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return std::nullopt;
        const auto end = stream.tellg();
        if (end <= 0 || static_cast<std::uintmax_t>(end) > (std::numeric_limits<std::size_t>::max)())
            return std::nullopt;
        auto storage = std::make_shared<std::vector<std::byte>>(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(storage->data()), static_cast<std::streamsize>(storage->size()));
        if (!stream)
            return std::nullopt;
        return lux::cxx::SharedBytes<>::fromOwner(storage, *storage);
    }

    [[nodiscard]] bool publishFile(const fs::path& target, std::span<const std::byte> bytes)
    {
        std::error_code error;
        if (!target.parent_path().empty())
        {
            fs::create_directories(target.parent_path(), error);
            if (error)
                return false;
        }
        auto temporary = target;
        temporary += ".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            if (!stream)
            {
                stream.close();
                fs::remove(temporary, error);
                return false;
            }
        }
#if defined(_WIN32)
        if (::MoveFileExW(
                temporary.c_str(),
                target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            ) == 0)
        {
            fs::remove(temporary, error);
            return false;
        }
#else
        fs::rename(temporary, target, error);
        if (error)
        {
            fs::remove(temporary, error);
            return false;
        }
#endif
        return true;
    }

    [[nodiscard]] lux::asset::AssetId deterministicId(
        std::string_view type,
        std::string_view options,
        lux::cxx::SharedBytes<> source
    ) noexcept
    {
        lux::cxx::algorithm::Sha256 hasher;
        hasher.update(type);
        hasher.update(options);
        hasher.update(source.view());
        const auto digest = hasher.digest();
        std::array<std::uint8_t, 16U> bytes{};
        for (std::size_t index = 0U; index < bytes.size(); ++index)
            bytes[index] = std::to_integer<std::uint8_t>(digest[index]);
        bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
        bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
        return lux::asset::AssetId{bytes};
    }

    [[nodiscard]] lux::asset::AssetInfo metadata(
        lux::asset::AssetId id,
        lux::asset::AssetTypeId type,
        const fs::path& source
    ) noexcept
    {
        lux::asset::AssetInfo result{};
        result.id = id;
        result.type = type;
        const std::string display = source.stem().string();
        const std::string origin = source.generic_string();
        std::memcpy(
            result.display_name.data(),
            display.data(),
            (std::min)(display.size(), result.display_name.size() - 1U)
        );
        std::memcpy(
            result.source_path.data(),
            origin.data(),
            (std::min)(origin.size(), result.source_path.size() - 1U)
        );
        return result;
    }

    [[nodiscard]] lux::asset::AssetDecodeLimits imageLimits(lux::cxx::SharedBytes<> image) noexcept
    {
        return lux::asset::AssetDecodeLimits{
            image.size(),
            image.size(),
            image.size() / 16U + 1U
        };
    }

    [[nodiscard]] int cookAsset(const Options& options)
    {
        const auto type = options.value("type");
        const auto source_text = options.value("source_path");
        const auto target_text = options.value("target_path");
        if (!type || !source_text || !target_text)
        {
            std::cerr << "import requires --type, --source_path and --target_path\n";
            return 2;
        }
        const fs::path source = *source_text;
        const fs::path target = *target_text;
        const auto source_bytes = readOwned(source);
        if (!source_bytes)
        {
            std::cerr << "cannot read source image\n";
            return 3;
        }

        std::vector<std::byte> encoded;
        if (*type == "texture")
        {
            const std::string format_text = options.value("texture_format").value_or("bc7_srgb");
            const std::string color_text = options.value("texture_color_space").value_or("srgb");
            const auto format = textureFormat(format_text);
            const auto color_space = textureColorSpace(color_text);
            if (!format || *format == lux::rdesc::ETexturePixelFormat::UNKNOWN || !color_space)
            {
                std::cerr << "unsupported texture format/color space\n";
                return 4;
            }
            const std::string identity_options = format_text + ":" + color_text +
                (options.has("no_mips") ? ":no-mips" : ":mips");
            const auto cooked = lux::toolchain::cookTexture(
                metadata(
                    deterministicId("texture", identity_options, *source_bytes),
                    lux::asset::TextureAsset::asset_type,
                    source
                ),
                *source_bytes,
                lux::toolchain::TextureCookConfiguration{
                    *format,
                    *color_space,
                    options.has("no_mips")
                }
            );
            if (!cooked)
            {
                std::cerr << "texture cook failed: " << static_cast<int>(cooked.error().code) << '\n';
                return 5;
            }
            const auto image = lux::asset::TAssetSerDeser<lux::asset::TextureAsset>::encode(
                **cooked,
                lux::asset::AssetEncodeLimits{(std::numeric_limits<std::size_t>::max)()}
            );
            if (!image)
                return 6;
            encoded = *image;
        }
        else if (*type == "shader")
        {
            const auto cooked = lux::toolchain::cookShader(
                metadata(
                    deterministicId("shader", "spirv", *source_bytes),
                    lux::asset::ShaderAsset::asset_type,
                    source
                ),
                *source_bytes
            );
            if (!cooked)
            {
                std::cerr << "shader cook failed: " << static_cast<int>(cooked.error().code) << '\n';
                return 7;
            }
            const auto image = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::encode(
                **cooked,
                lux::asset::AssetEncodeLimits{(std::numeric_limits<std::size_t>::max)()}
            );
            if (!image)
                return 8;
            encoded = *image;
        }
        else
        {
            std::cerr << "unsupported asset type; expected shader or texture\n";
            return 9;
        }
        if (!publishFile(target, encoded))
        {
            std::cerr << "cannot atomically publish target\n";
            return 10;
        }
        std::cout << "packed " << *type << " bytes=" << encoded.size() << " target=" << target.string() << '\n';
        return 0;
    }

    [[nodiscard]] int inspectAsset(const fs::path& path)
    {
        const auto bytes = readOwned(path);
        if (!bytes)
            return 2;
        const auto image = lux::asset::inspectCookedAssetImage(*bytes, imageLimits(*bytes));
        if (!image)
            return 3;
        const auto id = image->metadata().id;
        std::cout << "magic=0x" << std::hex << image->magic() << std::dec
                  << " version=" << image->version()
                  << " info=" << image->information().size()
                  << " data=" << image->data().size() << '\n';
        if (image->magic() == lux::asset::TextureAsset::primary_magic)
        {
            const auto texture = lux::asset::TAssetSerDeser<lux::asset::TextureAsset>::decode(
                id, *bytes, imageLimits(*bytes)
            );
            if (!texture)
                return 5;
            std::cout << "texture=" << (*texture)->data().width() << 'x' << (*texture)->data().height()
                      << " mips=" << (*texture)->data().mipCount() << '\n';
        }
        else if (image->magic() == lux::asset::ShaderAsset::primary_magic)
        {
            const auto shader = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
                id, *bytes, imageLimits(*bytes)
            );
            if (!shader)
                return 6;
            std::cout << "shader_entries=" << (*shader)->data().info.entry_points.size()
                      << " spirv=" << (*shader)->data().shader.size() << '\n';
        }
        else
        {
            std::cerr << "unsupported typed asset magic\n";
            return 7;
        }
        return 0;
    }

    [[nodiscard]] int embedShader(const Options& options)
    {
        const auto input_text = options.value("target_path");
        const auto output_text = options.value("source_path");
        const auto name = options.value("embed_name");
        const std::string name_space = options.value("embed_namespace").value_or("lux::render::builtin");
        if (!input_text || !output_text || !name)
            return 2;
        const auto bytes = readOwned(*input_text);
        if (!bytes)
            return 3;
        const auto image = lux::asset::inspectCookedAssetImage(*bytes, imageLimits(*bytes));
        if (!image || image->magic() != lux::asset::ShaderAsset::primary_magic)
            return 4;
        const auto shader = lux::asset::TAssetSerDeser<lux::asset::ShaderAsset>::decode(
            image->metadata().id,
            *bytes,
            imageLimits(*bytes)
        );
        if (!shader)
            return 5;
        const auto info = lux::rdesc::ShaderInfo::serialize((*shader)->data().info);
        const auto spirv = std::span<const std::byte>{
            static_cast<const std::byte*>((*shader)->data().shader.data()),
            (*shader)->data().shader.size()
        };

        std::ostringstream text;
        text << "// Auto-generated by lux_asset_packer --embed. Do not edit.\n#pragma once\n"
             << "#include <cstddef>\n#include <cstdint>\n\nnamespace " << name_space << " {\n\n";
        const auto emit = [&](std::string_view suffix, std::span<const std::byte> payload) {
            text << "alignas(4) inline constexpr std::uint8_t " << *name << suffix << "[] = {";
            for (std::size_t index = 0U; index < payload.size(); ++index)
            {
                if (index % 16U == 0U)
                    text << "\n    ";
                text << "0x" << std::hex << std::setw(2) << std::setfill('0')
                     << std::to_integer<unsigned int>(payload[index]) << std::dec;
                if (index + 1U != payload.size())
                    text << ',';
            }
            text << "\n};\ninline constexpr std::size_t " << *name << suffix << "_size = "
                 << payload.size() << "U;\n\n";
        };
        emit("_spirv", spirv);
        emit("_info", info);
        text << "} // namespace " << name_space << '\n';
        const std::string output = text.str();
        return publishFile(
            *output_text,
            std::as_bytes(std::span<const char>{output.data(), output.size()})
        ) ? 0 : 6;
    }

    [[nodiscard]] int packDirectory(const Options& options)
    {
        const auto source_text = options.value("source_path");
        const auto target_text = options.value("target_path");
        if (!source_text || !target_text)
            return 2;
        const fs::path source = *source_text;
        std::error_code error;
        if (!fs::is_directory(source, error) || error)
            return 3;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator iterator(source, error), end; iterator != end && !error;
             iterator.increment(error))
        {
            if (iterator->is_regular_file() && iterator->path().extension() == ".luxasset")
                files.push_back(iterator->path());
        }
        if (error || files.empty())
            return 4;
        std::sort(files.begin(), files.end(), [&](const fs::path& left, const fs::path& right) {
            return left.lexically_relative(source).generic_string() < right.lexically_relative(source).generic_string();
        });

        std::vector<lux::asset::PakWriteEntry> entries;
        for (const auto& file : files)
        {
            const auto bytes = readOwned(file);
            if (!bytes)
            {
                std::cerr << "invalid cooked asset: " << file.string() << '\n';
                return 5;
            }
            const auto image = lux::asset::inspectCookedAssetImage(*bytes, imageLimits(*bytes));
            if (!image)
            {
                std::cerr << "invalid cooked asset: " << file.string() << '\n';
                return 5;
            }
            auto relative = file.lexically_relative(source);
            relative.replace_extension();
            entries.push_back({
                image->metadata().id,
                image->magic(),
                relative.generic_string(),
                file,
                {}
            });
        }
        std::string message;
        const std::string mount_hint = options.value("mount_hint").value_or("/Game");
        if (!lux::asset::writePakFile(*target_text, std::move(entries), mount_hint, &message))
        {
            std::cerr << "pak write failed: " << message << '\n';
            return 6;
        }
        return 0;
    }

    [[nodiscard]] int inspectPak(const fs::path& path)
    {
        const auto info = lux::asset::inspectPak(path);
        if (!info)
        {
            std::cerr << info.error() << '\n';
            return 2;
        }
        std::cout << "mount=" << info->mount_hint << " entries=" << info->entries.size() << '\n';
        for (const auto& entry : info->entries)
            std::cout << entry.vpath << " size=" << entry.size << '\n';
        return 0;
    }

    void usage()
    {
        std::cerr << "lux_asset_packer --type shader|texture --source_path <path> --target_path <path>\n"
                  << "  [--texture_format <format>] [--texture_color_space srgb|linear|data] [--no_mips]\n"
                  << "  --inspect | --embed | --pack | --pak_inspect\n";
    }
} // namespace

int main(int argc, char** argv)
{
    const auto options = parseOptions(argc, argv);
    if (!options)
    {
        usage();
        return 1;
    }
    if (options->has("inspect"))
    {
        const auto target = options->value("target_path");
        return target ? inspectAsset(*target) : 1;
    }
    if (options->has("embed"))
        return embedShader(*options);
    if (options->has("pack"))
        return packDirectory(*options);
    if (options->has("pak_inspect"))
    {
        const auto target = options->value("target_path");
        return target ? inspectPak(*target) : 1;
    }
    return cookAsset(*options);
}
