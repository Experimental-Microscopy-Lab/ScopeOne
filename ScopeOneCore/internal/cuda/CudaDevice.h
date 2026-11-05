#pragma once


namespace scopeone::core::internal::cuda
{
    bool isCudaDeviceAvailable();
    int cudaDeviceCount();
}
