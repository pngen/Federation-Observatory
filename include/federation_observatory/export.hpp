// Federation Observatory - Apache License 2.0 - Copyright 2026 Summon Software Labs.
#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#  if defined(FO_BUILD_SHARED)
#    define FO_API __declspec(dllexport)
#  elif defined(FO_USE_SHARED)
#    define FO_API __declspec(dllimport)
#  else
#    define FO_API
#  endif
#  define FO_HIDDEN
#elif defined(__GNUC__) || defined(__clang__)
#  define FO_API __attribute__((visibility("default")))
#  define FO_HIDDEN __attribute__((visibility("hidden")))
#else
#  define FO_API
#  define FO_HIDDEN
#endif

#define FO_VERSION_MAJOR 1
#define FO_VERSION_MINOR 0
#define FO_VERSION_PATCH 0
#define FO_VERSION_STRING "1.0.0"
