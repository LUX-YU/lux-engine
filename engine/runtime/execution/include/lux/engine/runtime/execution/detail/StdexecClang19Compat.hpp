#pragma once

#include <stdexec/__detail/__config.hpp>

#if defined(__ANDROID__) && defined(__clang__) && __clang_major__ == 19
#undef STDEXEC_DEPRECATE_CONCEPT
#define STDEXEC_DEPRECATE_CONCEPT(message)
#endif
