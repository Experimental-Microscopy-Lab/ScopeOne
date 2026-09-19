#pragma once

#if defined(_WIN32)
#    if defined(SCOPEONE_INFERENCE_EXPORTS)
#        define SCOPEONE_INFERENCE_EXPORT __declspec(dllexport)
#    else
#        define SCOPEONE_INFERENCE_EXPORT __declspec(dllimport)
#    endif
#else
#    define SCOPEONE_INFERENCE_EXPORT __attribute__((visibility("default")))
#endif
