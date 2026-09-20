#pragma once
#if defined(_WIN32)
#if defined(PROBE_LIBRARY)
#define PROBE_PUBLIC __declspec(dllexport)
#else
#define PROBE_PUBLIC __declspec(dllimport)
#endif
#else
#define PROBE_PUBLIC
#endif
