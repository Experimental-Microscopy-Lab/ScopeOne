#pragma once

#if defined(_WIN32)
#    if defined(SCOPEONE_CUDA_EXPORTS)
#        define SCOPEONE_CUDA_EXPORT __declspec(dllexport)
#    else
#        define SCOPEONE_CUDA_EXPORT __declspec(dllimport)
#    endif
#else
#    define SCOPEONE_CUDA_EXPORT __attribute__((visibility("default")))
#endif
