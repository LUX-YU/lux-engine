#include "lux/reflection/LuxCxxParserImpl.hpp"
#include <numeric>

namespace lux::reflection
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

        TranslationUnit LuxCxxParserImpl::translate(const std::string& file_path, const std::vector<std::string>& commands)
        {
            static const char* parse_definition = "-D __PARSE_TIME__=1";
            CXTranslationUnit translation_unit;

            if(commands.size() > std::numeric_limits<int>::max())
            {
                return TranslationUnit(nullptr);
            }

            const int commands_size = static_cast<int>(commands.size());
            // +1 for add definition
            const char ** _c_commands = (const char **)malloc((commands_size + 1) * sizeof(char*));
            if (!_c_commands)
            {
                return TranslationUnit(nullptr);
            }

            for(size_t i = 0; i < commands_size ; i++)
            {
                _c_commands[i] = commands[i].c_str();
            }
            _c_commands[commands_size] = parse_definition;
            CXErrorCode error_code = clang_parseTranslationUnit2(
                clang_index,            // CXIndex
                file_path.c_str(),      // source_filename
                _c_commands,            // command line args
                commands_size + 1,      // num_command_line_args
                nullptr,                // unsaved_files
                0,                      // num_unsaved_files
                CXTranslationUnit_None, // option
                &translation_unit       // CXTranslationUnit
            );
            free(_c_commands);

            return TranslationUnit(error_code == CXErrorCode::CXError_Success ? translation_unit : nullptr);
        }
} // namespace lux::engine::core
