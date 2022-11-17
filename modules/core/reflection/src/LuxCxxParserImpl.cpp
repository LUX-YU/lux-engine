#include "lux-engine/core/reflection/LuxCxxParserImpl.hpp"

namespace lux::engine::core
{
        LuxCxxParserImpl::LuxCxxParserImpl()
        {
            /**
             * @brief information about clang_createIndex(int excludeDeclarationsFromPCH, int displayDiagnostics)
             * @param excludeDeclarationsFromPCH
             */
            clang_index = clang_createIndex(0, 1);
        }

        LuxCxxParserImpl::~LuxCxxParserImpl()
        {
            clang_disposeIndex(clang_index);
        }

        libclang::TranslationUnit LuxCxxParserImpl::translate(const std::string& file_path, const std::vector<std::string>& commands)
        {
            CXTranslationUnit translation_unit;
            const size_t commands_size = commands.size();
            const char ** _c_commands = (const char **)malloc(commands_size * sizeof(char*));
            for(size_t i = 0; i < commands_size ; i++)
            {
                _c_commands[i] = commands[i].c_str();
            }
            CXErrorCode error_code = clang_parseTranslationUnit2(
                clang_index,            // CXIndex
                file_path.c_str(),      // source_filename
                _c_commands,            // command line args
                commands_size,          // num_command_line_args
                nullptr,                // unsaved_files
                0,                      // num_unsaved_files
                CXTranslationUnit_None, // option
                &translation_unit       // CXTranslationUnit
            );
            delete _c_commands;
            return libclang::TranslationUnit(translation_unit);
        }
} // namespace lux::engine::core
