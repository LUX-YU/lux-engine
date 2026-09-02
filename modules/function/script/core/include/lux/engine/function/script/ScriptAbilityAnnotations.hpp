#pragma once

#if defined __LUX_PARSE_TIME__
#    define LUX_SCRIPT_META(...) __attribute__((annotate(#__VA_ARGS__)))
#else
#    define LUX_SCRIPT_META(...)
#endif

#define LUX_SCRIPT_ABILITY(...) LUX_SCRIPT_META(luxability::contract, __VA_ARGS__)
#define LUX_SCRIPT_QUERY(...) LUX_SCRIPT_META(luxability::method, kind = query, __VA_ARGS__)
#define LUX_SCRIPT_COMMAND(...) LUX_SCRIPT_META(luxability::method, kind = command, __VA_ARGS__)
#define LUX_SCRIPT_ASYNC(...) LUX_SCRIPT_META(luxability::method, kind = async_operation, __VA_ARGS__)
#define LUX_SCRIPT_PARAM(...) LUX_SCRIPT_META(luxability::param, __VA_ARGS__)
