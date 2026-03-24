#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
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

