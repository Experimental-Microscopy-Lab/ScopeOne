#pragma once

#include "scopeone/inference/OnnxRuntime.h"
#include "scopeone/inference/OnnxSession.h"

#include <onnxruntime_cxx_api.h>

namespace scopeone::inference
{
    struct OnnxRuntime::Impl
    {
        Impl();

        Ort::Env environment;
    };
}
