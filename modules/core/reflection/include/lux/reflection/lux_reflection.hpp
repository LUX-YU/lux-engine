#pragma once
#if defined __PARSE_TIME__
#define LUX_META(...) __attribute__((annotate("LUX::META;"#__VA_ARGS__)))
#else
#define LUX_META(...)
#endif