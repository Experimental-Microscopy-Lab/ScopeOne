#pragma once

// SCOPEONE_CORE_STATIC compiles a Core source into another binary, as DriverHost does with the
// plugin manifest, without importing it from the Core library
#if defined(SCOPEONE_CORE_STATIC)
#  define SCOPEONE_CORE_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#  if defined(SCOPEONE_CORE_EXPORTS)
#    define SCOPEONE_CORE_EXPORT __declspec(dllexport)
#  else
#    define SCOPEONE_CORE_EXPORT __declspec(dllimport)
#  endif
#else
#  if defined(__GNUC__) && __GNUC__ >= 4
#    define SCOPEONE_CORE_EXPORT __attribute__((visibility("default")))
#  else
#    define SCOPEONE_CORE_EXPORT
#  endif
#endif

