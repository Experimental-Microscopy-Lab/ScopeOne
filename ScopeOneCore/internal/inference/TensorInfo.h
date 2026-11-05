#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scopeone::inference
{
    enum class TensorElementType
    {
        Unknown,
        Float32,
        UInt8,
        Int8,
        UInt16,
        Int16,
        Int32,
        Int64,
        String,
        Boolean,
        Float16,
        Float64,
        UInt32,
        UInt64,
        Complex64,
        Complex128,
        BFloat16
    };

    struct TensorInfo
    {
        std::string name;
        TensorElementType elementType{TensorElementType::Unknown};
        std::vector<std::int64_t> shape;
    };

    struct FloatTensor
    {
        std::vector<std::int64_t> shape;
        std::vector<float> values;
    };
}
