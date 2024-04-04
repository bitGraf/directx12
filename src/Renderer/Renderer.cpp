#include "Renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Shellapi.h>

#if defined(min)
    #undef min
#endif

#if defined(max)
    #undef max
#endif

// Windows Runtime Library. Needed for Microsoft::WRL::ComPtr<> template class.
//#include <wrl.h>
//using namespace Microsoft::WRL;

// DirectX 12 specific headers.
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
 
// D3D12 extension library.
#include <directx/d3dx12.h>

// STL Headers
#include <algorithm>
#include <cassert>
#include <chrono>

#include "Helpers.h"
