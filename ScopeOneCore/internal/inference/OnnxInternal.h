#pragma once

#include "internal/inference/OnnxRuntime.h"
#include "internal/inference/OnnxSession.h"

#include <onnxruntime_cxx_api.h>

namespace scopeone::core::internal::inference
{
    struct OnnxRuntime::Impl
    {
        Impl();

        Ort::Env environment;
    };
}
