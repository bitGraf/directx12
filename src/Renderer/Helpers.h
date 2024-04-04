#pragma once

#define FAILED(hr) (((HRESULT)(hr)) < 0)

// From DXSampleHelper.h 
// Source: https://github.com/Microsoft/DirectX-Graphics-Samples
inline void ThrowIfFailed(long hr)
{
    if (FAILED(hr))
    {
        throw std::exception();
    }
}