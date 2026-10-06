#ifndef _LUX_EDIT_HISTORY_LIBRARY_VISIBILITY_H_
#define _LUX_EDIT_HISTORY_LIBRARY_VISIBILITY_H_

#if !defined LUX_EDIT_HISTORY_DLL_DISABLE
#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define LUX_EXPORT __attribute__ ((dllexport))
    #define LUX_IMPORT __attribute__ ((dllimport))
  #else
    #define LUX_EXPORT __declspec(dllexport)
    #define LUX_IMPORT __declspec(dllimport)
  #endif
  #ifdef LUX_EDIT_HISTORY_LIBRARY
    #define LUX_EDIT_HISTORY_PUBLIC LUX_EXPORT
  #else
    #define LUX_EDIT_HISTORY_PUBLIC LUX_IMPORT
  #endif
  #define LUX_EDIT_HISTORY_PUBLIC_TYPE LUX_PUBLIC
  #define LUX_LOCAL
#else
  #define LUX_EXPORT __attribute__ ((visibility("default")))
  #define LUX_IMPORT
  #if __GNUC__ >= 4
    #define LUX_EDIT_HISTORY_PUBLIC __attribute__ ((visibility("default")))
    #define LUX_LOCAL  __attribute__ ((visibility("hidden")))
  #else
    #define LUX_EDIT_HISTORY_PUBLIC
    #define LUX_LOCAL
  #endif
  #define LUX_EDIT_HISTORY_PUBLIC_TYPE
#endif
#else
	#define LUX_EDIT_HISTORY_PUBLIC
#endif

#endif
