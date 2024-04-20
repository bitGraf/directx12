#include "Renderer.h"

#include "Platform/platform.h"
#include "Core/Asserts.h"

// DirectX 12 headers.
#include <directx/d3d12.h>
#include <directx/d3dx12.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include "Renderer/DDSTextureLoader12.h"

#if defined(min)
#undef min
#endif


#if defined(max)
#undef max
#endif

#include <vector>
#include <chrono>

#include "Core/Logger.h"
#include "laml/laml.hpp"

#define str(x) #x
#define ToString(x) str(x)


#define MAX_OBJECTS 1024
#define MAX_OBJECTS_STR ToString(MAX_OBJECTS)

struct cbLayout_PerFrame {
    laml::Mat4 r_Projection;
    laml::Mat4 r_View;
    laml::Vec4 r_FogColor;
    real32     r_FogStart;
    real32     r_FogEnd;
};
struct cbLayout_PerModel {
    laml::Mat4 r_Model[MAX_OBJECTS];
};

struct DX12State {
    // DirectX 12 Objects
    Microsoft::WRL::ComPtr<ID3D12Debug>                 DebugController;
    DWORD                                               callback_cookie;

    Microsoft::WRL::ComPtr<ID3D12Device5>               Device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>          CommandQueue_Direct;
    //Microsoft::WRL::ComPtr<ID3D12CommandQueue>          CommandQueue_Copy;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4>  CmdList_Direct;

    Microsoft::WRL::ComPtr<IDXGISwapChain4>             SwapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource>              DepthStencilBuffer;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>         RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>         PSO_Standard;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>         PSO_Blend;

    // Descriptor Heaps
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        RTVDescriptorHeap;
    uint32                                              RTVDescriptorSize;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        DSVDescriptorHeap;
    uint32                                              DSVDescriptorSize;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        CBVDescriptorHeap;
    uint32                                              CBVDescriptorSize;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        SamDescriptorHeap;


    // mesh and data
    Microsoft::WRL::ComPtr<ID3D12Resource>              TriangleVB;
    D3D12_VERTEX_BUFFER_VIEW                            TriangleVBView;

    Microsoft::WRL::ComPtr<ID3D12Resource>              ScreenVB;
    D3D12_VERTEX_BUFFER_VIEW                            ScreenVBView;

    // texture
    Microsoft::WRL::ComPtr<ID3D12Resource>              TextureMetal;
    Microsoft::WRL::ComPtr<ID3D12Resource>              TextureChainlink;

    // Frame Resources
    uint32                                              frame_idx;
    static const uint8                                  num_frames_in_flight = 3;
    Microsoft::WRL::ComPtr<ID3D12Fence>                 Fence;
    uint64                                              CurrentFence = 0;
    struct FrameResources {
        Microsoft::WRL::ComPtr<ID3D12Resource>          BackBuffer;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>  CommandAllocator;
        
        Microsoft::WRL::ComPtr<ID3D12Resource>          upload_cbuffer_PerFrame;
        BYTE*                                           upload_cbuffer_PerFrame_mapped = nullptr;

        Microsoft::WRL::ComPtr<ID3D12Resource>          upload_cbuffer_PerModel;
        BYTE*                                           upload_cbuffer_PerModel_mapped = nullptr;

        uint64                                          FenceValue = 0;

        Microsoft::WRL::ComPtr<ID3D12Resource>          DynamicVB;
        D3D12_VERTEX_BUFFER_VIEW                        DynamicVBView;
        BYTE*                                           DynamicVB_mapped = nullptr;
    } frames[DX12State::num_frames_in_flight];
};
global_variable DX12State dx12;

struct RenderState {
    uint32 render_width  = 800;
    uint32 render_height = 800;
};
global_variable RenderState render;

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 color;
    DirectX::XMFLOAT2 texcoord;
};
const uint32 num_circle_verts = 16;
void generate_circle_verts(Vertex* verts, uint32 N, real32 t) {
    // for each edge vertex, we create a triangle with the center (0,0), 
    // that vertex, and the next vertex. If we are on vertex N-1, 'next' vertex
    // is vertex 0.
    real32 dtheta = 360.0f / static_cast<real32>(N);
    for (uint32 n = 0; n < N; n++) {
        uint32 next = n + 1;
        if (next == N) next = 0;

        real32 theta1 = t + n    * dtheta;
        real32 radius1 = 0.3f + 0.15f * laml::cosd(t*theta1/10.0f);
        real32 theta2 = t + next * dtheta;
        real32 radius2 = 0.3f + 0.15f * laml::cosd(t*theta2/10.0f);

        float x, y;
        verts[3*n + 0].position = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
        verts[3*n + 0].color    = DirectX::XMFLOAT3(1.0f, 0.6f, 0.6f);
        x = verts[3*n].position.x;
        y = verts[3*n].position.y;
        verts[3*n + 0].texcoord = DirectX::XMFLOAT2(0.5f*(x + 1.0f), 0.5f*(y + 1.0f));

        verts[3*n + 1].position = DirectX::XMFLOAT3(-radius1*laml::sind(theta1), radius1*laml::cosd(theta1), 0.0f);
        verts[3*n + 1].color    = DirectX::XMFLOAT3(0.6f, 1.0f, 0.6f);
        x = verts[3*n + 1].position.x;
        y = verts[3*n + 1].position.y;
        verts[3*n + 1].texcoord = DirectX::XMFLOAT2(0.5f*(x + 1.0f), 0.5f*(y + 1.0f));

        verts[3*n + 2].position = DirectX::XMFLOAT3(-radius1*laml::sind(theta2), radius1*laml::cosd(theta2), 0.0f);
        verts[3*n + 2].color    = DirectX::XMFLOAT3(0.6f, 0.6f, 1.0f);
        x = verts[3*n + 2].position.x;
        y = verts[3*n + 2].position.y;
        verts[3*n + 2].texcoord = DirectX::XMFLOAT2(0.5f*(x + 1.0f), 0.5f*(y + 1.0f));
    }
}

// callbacks
void d3d_debug_msg_callback(D3D12_MESSAGE_CATEGORY Category, 
                            D3D12_MESSAGE_SEVERITY Severity, 
                            D3D12_MESSAGE_ID ID, 
                            LPCSTR pDescription, 
                            void* pContext);

// flush the current command queue and for for it to finish.
void FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
    dx12.frames[dx12.frame_idx].FenceValue = ++dx12.CurrentFence;

    // Add an instruction to the command queue to set a new fence point.  Because we 
    // are on the GPU timeline, the new fence point won't be set until the GPU finishes
    // processing all the commands prior to this Signal().
    if FAILED(dx12.CommandQueue_Direct->Signal(dx12.Fence.Get(), dx12.frames[dx12.frame_idx].FenceValue)) {
        RH_FATAL("Failed to signal");
    }

	// Wait until the GPU has completed commands up to this fence point.
    if(dx12.Fence->GetCompletedValue() < dx12.frames[dx12.frame_idx].FenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

        // Fire event when GPU hits current fence.  
        if FAILED(dx12.Fence->SetEventOnCompletion(dx12.frames[dx12.frame_idx].FenceValue, eventHandle)) {
            RH_FATAL("Failed to set event");
        }

        // Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
	}
}

bool init_renderer() {
    using namespace Microsoft::WRL;

    // Enable D3D12 debug layer
#if defined(DEBUG) || defined(_DEBUG)
    if FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&dx12.DebugController))) {
        RH_FATAL("Could not get debug interface!");
        return false;
    }
    dx12.DebugController->EnableDebugLayer();
    RH_INFO("Debug Layer Enabled");
#endif

    // Create DXGI Factory
    ComPtr<IDXGIFactory4> factory;
    {
        UINT createFactoryFlags = 0;
#if defined(_DEBUG)
        createFactoryFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif

        if FAILED(CreateDXGIFactory2(createFactoryFlags, IID_PPV_ARGS(&factory))) {
            RH_FATAL("Failed to create factory...");
            return false;
        }
    }

    // Find adapter with the highest vram
    ComPtr<IDXGIAdapter1> adapter;
    uint32 max_idx = 0;
    uint64 max_vram = 0;
    {
        RH_INFO("------ Display Adapters ------------------------");

        uint32 i = 0;
        while (factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            RH_TRACE("Device %d: %ls", i, desc.Description);
            if (desc.DedicatedVideoMemory > max_vram) {
                max_vram = desc.DedicatedVideoMemory;
                max_idx = i;
            }

            ++i;
        }

        // get the best choice now
        if (factory->EnumAdapters1(max_idx, &adapter) == DXGI_ERROR_NOT_FOUND) {
            RH_FATAL("could not find the best Device...\n");
            return false;
        }

        // print out description of selected adapter
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        RH_INFO("Chosen Device: '%ls'"
        "\n         VideoMemory:  %.1llf GB"
        "\n         SystemMemory: %.1llf GB"
                , desc.Description, ((double)desc.DedicatedVideoMemory) / (1024.0*1024.0*1024.0));
    }

    // create Device
    {
        if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&dx12.Device))) {
            if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dx12.Device))) {
                if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dx12.Device))) {
                    if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&dx12.Device))) {
                        if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx12.Device))) {
                            RH_FATAL("No feature levels supported, could not create a DX12 Device!");
                            return false;
                        } else {
                            RH_INFO("Created a DX12 Device with Feature Level: 11_0");
                        }
                    } else {
                        RH_INFO("Created a DX12 Device with Feature Level: 11_1");
                    }
                } else {
                    RH_INFO("Created a DX12 Device with Feature Level: 12_0");
                }
            } else {
                RH_INFO("Created a DX12 Device with Feature Level: 12_1");
            }
        } else {
            RH_INFO("Created a DX12 Device with Feature Level: 12_2");
        }

        static const D3D_FEATURE_LEVEL FEATURE_LEVELS_ARRAY[] =
        {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        static const D3D_FEATURE_LEVEL MAX_FEATURE_LEVEL = D3D_FEATURE_LEVEL_12_2;
        static const D3D_FEATURE_LEVEL MIN_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;

        D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {
            _countof(FEATURE_LEVELS_ARRAY), FEATURE_LEVELS_ARRAY, MAX_FEATURE_LEVEL
        };
        dx12.Device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));

        // check for raytracing support
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
        dx12.Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
        if (opt5.RaytracingTier) {
            RH_INFO("Feature: Raytracing Tier: %.1f", (real32)opt5.RaytracingTier / 10.0f);
        }

        // enable debug messages
#if defined(_DEBUG)
        ComPtr<ID3D12InfoQueue> pInfoQueue;
        if (SUCCEEDED(dx12.Device->QueryInterface(IID_PPV_ARGS(&pInfoQueue))))
        //if (SUCCEEDED(dx12.Device.As(&pInfoQueue)))
        {
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
            pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);

            // Suppress whole categories of messages
            //D3D12_MESSAGE_CATEGORY Categories[] = {};

            // Suppress messages based on their severity level
            D3D12_MESSAGE_SEVERITY Severities[] =
            {
                D3D12_MESSAGE_SEVERITY_INFO
            };

            // Suppress individual messages by their ID
            D3D12_MESSAGE_ID DenyIds[] = {
                //D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,   // I'm really not sure how to avoid this message.
                D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,                         // This warning occurs when using capture frame while graphics debugging.
                D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,                       // This warning occurs when using capture frame while graphics debugging.
            };

            D3D12_INFO_QUEUE_FILTER NewFilter = {};
            //NewFilter.DenyList.NumCategories = _countof(Categories);
            //NewFilter.DenyList.pCategoryList = Categories;
            NewFilter.DenyList.NumSeverities = _countof(Severities);
            NewFilter.DenyList.pSeverityList = Severities;
            NewFilter.DenyList.NumIDs = _countof(DenyIds);
            NewFilter.DenyList.pIDList = DenyIds;

            if (FAILED(pInfoQueue->PushStorageFilter(&NewFilter))) {
                RH_FATAL("Failed to setup info-queue for debug messages");
                return false;
            }

            // THIS FEATURE REQUIRES WINDOWS BUILD 20236 OR HIGHER!!!
            // :(
            /*
            ComPtr<ID3D12InfoQueue1> pInfoQueue1;
            if (SUCCEEDED(pInfoQueue->QueryInterface(IID_PPV_ARGS(&pInfoQueue1)))) {
                D3D12_MESSAGE_CALLBACK_FLAGS callback_flags = D3D12_MESSAGE_CALLBACK_FLAG_NONE;
                if SUCCEEDED(pInfoQueue1->RegisterMessageCallback(d3d_debug_msg_callback, callback_flags, NULL, &dx12.callback_cookie)) {
                    RH_FATAL("Could not register msg callback...");
                    return false;
                }
            } else {
                RH_FATAL("Could not create info-queue");
                return false;
            }
            */
        } else {
            RH_FATAL("Could not create info-queue");
            return false;
        }
#endif
    }

    // check for MSAA quality level
    {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qual_level;
        qual_level.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        qual_level.SampleCount = 4;
        qual_level.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
        qual_level.NumQualityLevels = 0;
        dx12.Device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qual_level, sizeof(qual_level));

        RH_INFO("MSAA x4 Quality Level: %u", qual_level.NumQualityLevels);
    }

    // Check for root signature version 1.2
    {
        D3D12_FEATURE_DATA_ROOT_SIGNATURE sig;
        sig.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_2;
        if (FAILED(dx12.Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &sig, sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)))) {

            // 1.2 not valid. Check for 1.1
            sig.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
            if (FAILED(dx12.Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &sig, sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)))) {

                // 1.1 not valid. Check for 1.0
                sig.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
                if (FAILED(dx12.Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &sig, sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE)))) {
            
                    // 1.0 not valid. This shouldn't happen?
                    RH_FATAL("Root Signature 1.0 not supported,");
                    return false;
                } else {
                    RH_INFO("Feature: Root Signature 1.0");
                }
            } else {
                RH_INFO("Feature: Root Signature 1.1");
            }
        } else {
            RH_INFO("Feature: Root Signature 1.2");
        }
    }

    // Create Command Queues
    {
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};

        // copy queue
        //queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_COPY;
        //queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        //queue_desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        //queue_desc.NodeMask = 0;
        //
        //if (FAILED(dx12.Device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&dx12.CommandQueue_Copy)))) {
        //    RH_FATAL("Could not create copy queue");
        //    return false;
        //}

        // direct queue
        queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_desc.NodeMask = 0;

        if (FAILED(dx12.Device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&dx12.CommandQueue_Direct)))) {
            RH_FATAL("Could not create direct queue");
            return false;
        }
    }

    // Create Swap Chain with 3 frames in flight
    {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width         = render.render_width;
        swapChainDesc.Height        = render.render_height;
        swapChainDesc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.Stereo        = FALSE;
        swapChainDesc.SampleDesc    = { 1, 0 };
        swapChainDesc.BufferUsage   = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount   = dx12.num_frames_in_flight;
        swapChainDesc.Scaling       = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect    = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode     = DXGI_ALPHA_MODE_UNSPECIFIED;
        // It is recommended to always allow tearing if tearing support is available.
        //swapChainDesc.Flags = CheckTearingSupport() ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> swapChain1;
        HWND window = (HWND)platform_get_window_handle();
        if FAILED(factory->CreateSwapChainForHwnd(
                  dx12.CommandQueue_Direct.Get(),
                  window,
                  &swapChainDesc,
                  nullptr,
                  nullptr,
                  &swapChain1)) {
            RH_FATAL("Could not create swap chain");
            return false;
        }

        // Disable the Alt+Enter fullscreen toggle feature. Switching to fullscreen
        // will be handled manually.
        if FAILED(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER)) {
            RH_FATAL("Could not disable alt-enter");
            return false;
        }

        if FAILED(swapChain1.As(&dx12.SwapChain)) {
            RH_FATAL("Could not turn swapchain1 into swapchain4");
            return false;
        }

        dx12.frame_idx = dx12.SwapChain->GetCurrentBackBufferIndex();
    }

    // Query Descriptor Heap Sizes
    dx12.RTVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dx12.DSVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    dx12.CBVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create Descriptor Heap
    { // RTV
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = dx12.num_frames_in_flight;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.RTVDescriptorHeap))) {
            RH_FATAL("Failed making descriptor heap");
            return false;
        }
    }
    { // CBV/SRV/UAV
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 2;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask       = 0;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.CBVDescriptorHeap))) {
            RH_FATAL("Failed making descriptor heap");
            return false;
        }
    }
    { // DSV
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask       = 0;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.DSVDescriptorHeap))) {
            RH_FATAL("Failed making descriptor heap");
            return false;
        }
    }
    { // Samplers
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc.NodeMask       = 0;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.SamDescriptorHeap))) {
            RH_FATAL("Failed making descriptor heap");
            return false;
        }
    }

    // setup back buffers
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx12.RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < dx12.num_frames_in_flight; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            if FAILED(dx12.SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer))) {
                RH_FATAL("Failed getting backbuffer %d", i);
                return false;
            }

            dx12.Device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

            dx12.frames[i].BackBuffer = backBuffer;

            rtvHandle.Offset(dx12.RTVDescriptorSize);

            //dx12.frames[i].FenceValue = i;
        }
    }

    // command allocators - one per frame
    for (uint8 n = 0; n < dx12.num_frames_in_flight; n++) {
        if (FAILED(dx12.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx12.frames[n].CommandAllocator)))) {
            RH_FATAL("Could not create command allocators");
            return false;
        }
    }

    // create command list for current backbuffer
    {
        if FAILED(dx12.Device->CreateCommandList(0, 
                                            D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                            dx12.frames[dx12.frame_idx].CommandAllocator.Get(), 
                                            nullptr, 
                                            IID_PPV_ARGS(dx12.CmdList_Direct.GetAddressOf()))) {
            RH_FATAL("Could not create command list!");
            return false;
        }

        // start it closed, since each update cycle starts with reset, and it needs to be closed.
        dx12.CmdList_Direct->Close();
    }

    // create depth/stencil buffer
    {
        DXGI_FORMAT depth_fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;
        D3D12_RESOURCE_DESC desc;
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width  = render.render_width;
        desc.Height = render.render_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = depth_fmt;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear;
        clear.Format = depth_fmt;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;
        CD3DX12_HEAP_PROPERTIES p(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(dx12.Device->CreateCommittedResource(&p,
                                                  D3D12_HEAP_FLAG_NONE,
                                                  &desc,
                                                  D3D12_RESOURCE_STATE_COMMON,
                                                  &clear,
                                                  IID_PPV_ARGS(dx12.DepthStencilBuffer.GetAddressOf())))) {
            RH_FATAL("Failed to create depth/stencil buffer");
            return false;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
        dsvDesc.Flags              = D3D12_DSV_FLAG_NONE;
        dsvDesc.ViewDimension      = D3D12_DSV_DIMENSION_TEXTURE2D;
        dsvDesc.Format             = depth_fmt;
        dsvDesc.Texture2D.MipSlice = 0;

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dx12.DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        dx12.Device->CreateDepthStencilView(dx12.DepthStencilBuffer.Get(), &dsvDesc, dsv);
    }

    // create fence
    if FAILED(dx12.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12.Fence))) {
        RH_FATAL("Could not create fence!");
        return false;
    }

    // create constant buffer - PerFrame
    for (uint32 n = 0; n < dx12.num_frames_in_flight; n++) {
        {
            uint32 cbSize = sizeof(cbLayout_PerFrame);
            cbSize = (cbSize + 255) & ~255; // make it a multiple of 256, minimum alloc size

            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
            dx12.Device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
                                                 &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&dx12.frames[n].upload_cbuffer_PerFrame));

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
            cbv_desc.BufferLocation = dx12.frames[n].upload_cbuffer_PerFrame->GetGPUVirtualAddress();
            cbv_desc.SizeInBytes = cbSize;

            CD3DX12_CPU_DESCRIPTOR_HANDLE heap(dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                               0, dx12.CBVDescriptorSize);
            //dx12.Device->CreateConstantBufferView(&cbv_desc, heap);

            // map the cbuffer so we have a memory address.
            // Keep this mapped until the program exits
            dx12.frames[n].upload_cbuffer_PerFrame->Map(0, 
                                                        nullptr, 
                                                        reinterpret_cast<void**>(&dx12.frames[n].upload_cbuffer_PerFrame_mapped));
        }

        // create constant buffer - PerModel
        {
            uint32 cbSize = sizeof(cbLayout_PerModel);
            cbSize = (cbSize + 255) & ~255; // make it a multiple of 256, minimum alloc size

            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
            dx12.Device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
                                                 &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&dx12.frames[n].upload_cbuffer_PerModel));

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
            cbv_desc.BufferLocation = dx12.frames[n].upload_cbuffer_PerModel->GetGPUVirtualAddress();
            cbv_desc.SizeInBytes = cbSize;

            CD3DX12_CPU_DESCRIPTOR_HANDLE heap(dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                               1,
                                               dx12.CBVDescriptorSize);
            //dx12.Device->CreateConstantBufferView(&cbv_desc, heap);

            // map the cbuffer so we have a memory address.
            // Keep this mapped until the program exits
            dx12.frames[n].upload_cbuffer_PerModel->Map(0, 
                                                        nullptr, 
                                                        reinterpret_cast<void**>(&dx12.frames[n].upload_cbuffer_PerModel_mapped));

            // fill each buffer with an identity matrix
            cbLayout_PerModel cbdata;
            for (uint32 m = 0; m < MAX_OBJECTS; m++) {
                laml::identity(cbdata.r_Model[m]);
            }
            memcpy(dx12.frames[n].upload_cbuffer_PerModel_mapped, &cbdata, sizeof(cbdata));
        }
    }

    // create root signature
    {
        D3D12_ROOT_PARAMETER root_parameters[5];
        
        root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        root_parameters[0].Constants.ShaderRegister = 0;
        root_parameters[0].Constants.RegisterSpace  = 0;
        root_parameters[0].Constants.Num32BitValues = 4;

        root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_parameters[1].Descriptor.ShaderRegister = 1;
        root_parameters[1].Descriptor.RegisterSpace  = 0;

        root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_parameters[2].Descriptor.ShaderRegister = 2;
        root_parameters[2].Descriptor.RegisterSpace  = 0;

        D3D12_DESCRIPTOR_RANGE srv_range = {};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.BaseShaderRegister = 0;
        srv_range.NumDescriptors = 1;
        srv_range.RegisterSpace = 0;
        srv_range.OffsetInDescriptorsFromTableStart = 0;
        root_parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_parameters[3].DescriptorTable.NumDescriptorRanges = 1;
        root_parameters[3].DescriptorTable.pDescriptorRanges = &srv_range;

        D3D12_DESCRIPTOR_RANGE sampler_range = {};
        sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        sampler_range.BaseShaderRegister = 0;
        sampler_range.NumDescriptors = 1;
        sampler_range.RegisterSpace = 0;
        sampler_range.OffsetInDescriptorsFromTableStart = 0;
        root_parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_parameters[4].DescriptorTable.NumDescriptorRanges = 1;
        root_parameters[4].DescriptorTable.pDescriptorRanges = &sampler_range;

        CD3DX12_ROOT_SIGNATURE_DESC root_sig_desc(_countof(root_parameters), root_parameters, 
                                                  0, nullptr, // static samplers
                                                  D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized_root_sig = nullptr;
        ComPtr<ID3DBlob> error_blob          = nullptr;
        if FAILED(D3D12SerializeRootSignature(&root_sig_desc,
                                              D3D_ROOT_SIGNATURE_VERSION_1_0,
                                              serialized_root_sig.GetAddressOf(),
                                              error_blob.GetAddressOf())) {
            RH_FATAL("Could not serialize root signature");
            RH_FATAL("DxError: %s", error_blob->GetBufferPointer());
            return false;
        }

        if FAILED(dx12.Device->CreateRootSignature(0,
                                                   serialized_root_sig->GetBufferPointer(),
                                                   serialized_root_sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&dx12.RootSignature))) {
            RH_FATAL("Could not create root signature");
            return false;
        }
    }

    // load mesh data
    auto alloc = dx12.frames[dx12.frame_idx].CommandAllocator;
    auto cmdlist = dx12.CmdList_Direct;

    alloc->Reset();
    cmdlist->Reset(alloc.Get(), nullptr);
    D3D12_INPUT_ELEMENT_DESC vert_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    // Define the geometry for a static triangle.
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_triangleVB;
    {
        Vertex vertex_data[] =
        {
            { {  0.0000f,  0.250f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f } },
            { {  0.2165f, -0.125f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
            { { -0.2165f, -0.125f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } }
        };

        const UINT buf_size = sizeof(vertex_data);

        // you can only directly copy data from CPU to GPU into the upload heap.
        // ideally we want this to be in the default buffer, so to do that:
        // 1. create the actual buffer in the default heap
        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
        dx12.Device->CreateCommittedResource(
            &prop, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&dx12.TriangleVB));

        // 2. create a temp upload buffer in the upload heap
        prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
        dx12.Device->CreateCommittedResource(
            &prop, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, 
            nullptr, IID_PPV_ARGS(&upload_buffer_triangleVB));

        // 3. describe the data we wish to copy to the gpu
        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData      = vertex_data;
        subResourceData.RowPitch   = buf_size;
        subResourceData.SlicePitch = subResourceData.RowPitch;

        // 4. schedule the copy into the default resource
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TriangleVB.Get(), 
                                                                              D3D12_RESOURCE_STATE_COMMON, 
                                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        cmdlist->ResourceBarrier(1, &barrier);
        UpdateSubresources<1>(cmdlist.Get(), dx12.TriangleVB.Get(), upload_buffer_triangleVB.Get(), 0, 0, 1, &subResourceData);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TriangleVB.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdlist->ResourceBarrier(1, &barrier);

        // initialize the vertex buffer view.
        dx12.TriangleVBView.BufferLocation = dx12.TriangleVB->GetGPUVirtualAddress();
        dx12.TriangleVBView.StrideInBytes  = sizeof(Vertex);
        dx12.TriangleVBView.SizeInBytes    = buf_size;

        // Transition the resource from its initial state to be used as a depth buffer.
        auto t = CD3DX12_RESOURCE_BARRIER::Transition(dx12.DepthStencilBuffer.Get(),
                                                      D3D12_RESOURCE_STATE_COMMON, 
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdlist->ResourceBarrier(1, &t);
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_sceenVB;
    {
        float s = 0.3f;
        Vertex vertex_data[] =
        {
            { {  s, -s, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f } },
            { {  s,  s, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },
            { { -s, -s, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
            { { -s,  s, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } }
        };

        const UINT buf_size = sizeof(vertex_data);

        // you can only directly copy data from CPU to GPU into the upload heap.
        // ideally we want this to be in the default buffer, so to do that:
        // 1. create the actual buffer in the default heap
        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
        dx12.Device->CreateCommittedResource(
            &prop, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&dx12.ScreenVB));

        // 2. create a temp upload buffer in the upload heap
        prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
        dx12.Device->CreateCommittedResource(
            &prop, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, 
            nullptr, IID_PPV_ARGS(&upload_buffer_sceenVB));

        // 3. describe the data we wish to copy to the gpu
        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData      = vertex_data;
        subResourceData.RowPitch   = buf_size;
        subResourceData.SlicePitch = subResourceData.RowPitch;

        // 4. schedule the copy into the default resource
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.ScreenVB.Get(), 
                                                                              D3D12_RESOURCE_STATE_COMMON, 
                                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        cmdlist->ResourceBarrier(1, &barrier);
        UpdateSubresources<1>(cmdlist.Get(), dx12.ScreenVB.Get(), upload_buffer_sceenVB.Get(), 0, 0, 1, &subResourceData);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.ScreenVB.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdlist->ResourceBarrier(1, &barrier);

        // initialize the vertex buffer view.
        dx12.ScreenVBView.BufferLocation = dx12.ScreenVB->GetGPUVirtualAddress();
        dx12.ScreenVBView.StrideInBytes  = sizeof(Vertex);
        dx12.ScreenVBView.SizeInBytes    = buf_size;
    }

    // Define texture for triangles
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_Texture_metal;
    {
        std::unique_ptr<uint8_t[]> ddsData;
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        ID3D12Resource* tex_ptr;
        HRESULT res = DirectX::LoadDDSTextureFromFile(
            dx12.Device.Get(),
            L"../Data/metal.dds",
            &tex_ptr,
            ddsData,
            subresources);

        if FAILED(res) {
            RH_FATAL("Could not load .dds texture!");
            return false;
        }

        dx12.TextureMetal.Attach(tex_ptr);

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(tex_ptr, 0, static_cast<UINT>(subresources.size()));

        // create an upload buffer and upload to the default heap
        {
            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
            dx12.Device->CreateCommittedResource(
                &prop, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&upload_buffer_Texture_metal));
        }

        // subresources are already defined
        // Just need to schedule the copy into the default resource
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TextureMetal.Get(), 
                                                                              D3D12_RESOURCE_STATE_COMMON, 
                                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        cmdlist->ResourceBarrier(1, &barrier);
        UpdateSubresources(cmdlist.Get(),                                   // cmdList
                           dx12.TextureMetal.Get(),                              // Destination
                           upload_buffer_Texture_metal.Get(),                     // Intermediate
                           0,                                               // IntermediateOffset
                           0,                                               // FirstSubresource
                           static_cast<UINT>(subresources.size()),          // NumSubresources
                           subresources.data());                            // Subresources
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TextureMetal.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdlist->ResourceBarrier(1, &barrier);

        // create SRV descriptors
        CD3DX12_CPU_DESCRIPTOR_HANDLE hdesc(dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                            0, dx12.CBVDescriptorSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = static_cast<UINT>(subresources.size());
        desc.Texture2D.PlaneSlice = 0;
        desc.Texture2D.ResourceMinLODClamp = 0.0;

        dx12.Device->CreateShaderResourceView(dx12.TextureMetal.Get(), &desc, hdesc);

        // create sampler
        D3D12_SAMPLER_DESC sdesc = {};
        sdesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sdesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sdesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sdesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sdesc.MinLOD = 0;
        sdesc.MaxLOD = D3D12_FLOAT32_MAX;
        sdesc.MipLODBias = 0.0f;
        sdesc.MaxAnisotropy = 1;
        sdesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

        dx12.Device->CreateSampler(&sdesc, dx12.SamDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer_Texture_chainlink;
    {
        std::unique_ptr<uint8_t[]> ddsData;
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        ID3D12Resource* tex_ptr;
        HRESULT res = DirectX::LoadDDSTextureFromFile(
            dx12.Device.Get(),
            //L"../Data/chainlink.dds",
            L"../Data/WireFence.dds",
            &tex_ptr,
            ddsData,
            subresources);

        if FAILED(res) {
            RH_FATAL("Could not load .dds texture!");
            return false;
        }

        dx12.TextureChainlink.Attach(tex_ptr);

        const UINT64 uploadBufferSize = GetRequiredIntermediateSize(tex_ptr, 0, static_cast<UINT>(subresources.size()));

        // create an upload buffer and upload to the default heap
        {
            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
            dx12.Device->CreateCommittedResource(
                &prop, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&upload_buffer_Texture_chainlink));
        }

        // subresources are already defined
        // Just need to schedule the copy into the default resource
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TextureChainlink.Get(), 
                                                                              D3D12_RESOURCE_STATE_COMMON, 
                                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        cmdlist->ResourceBarrier(1, &barrier);
        UpdateSubresources(cmdlist.Get(),                                   // cmdList
                           dx12.TextureChainlink.Get(),                              // Destination
                           upload_buffer_Texture_chainlink.Get(),                     // Intermediate
                           0,                                               // IntermediateOffset
                           0,                                               // FirstSubresource
                           static_cast<UINT>(subresources.size()),          // NumSubresources
                           subresources.data());                            // Subresources
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.TextureChainlink.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        cmdlist->ResourceBarrier(1, &barrier);

        // create SRV descriptors
        CD3DX12_CPU_DESCRIPTOR_HANDLE hdesc(dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                            1, dx12.CBVDescriptorSize);

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.Format = DXGI_FORMAT_BC3_UNORM;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = static_cast<UINT>(subresources.size());
        desc.Texture2D.PlaneSlice = 0;
        desc.Texture2D.ResourceMinLODClamp = 0.0;

        dx12.Device->CreateShaderResourceView(dx12.TextureChainlink.Get(), &desc, hdesc);
    }

    // Define the geometry for a dynamic vertex buffer
    {
        // generate vertices for a circle
        Vertex circle_verts[num_circle_verts * 3];

        generate_circle_verts(circle_verts, num_circle_verts, 0.0f);
        const UINT buf_size = sizeof(circle_verts);

        // Since this data changes, we store it in the upload heap always

        for (uint32 n = 0; n < dx12.num_frames_in_flight; n++) {
            auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
            dx12.Device->CreateCommittedResource(
                &prop, D3D12_HEAP_FLAG_NONE,
                &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr, IID_PPV_ARGS(&dx12.frames[n].DynamicVB));

            dx12.frames[n].DynamicVBView.BufferLocation = dx12.frames[n].DynamicVB->GetGPUVirtualAddress();
            dx12.frames[n].DynamicVBView.StrideInBytes = sizeof(Vertex);
            dx12.frames[n].DynamicVBView.SizeInBytes = buf_size;

            dx12.frames[n].DynamicVB->Map(0,
                                                       nullptr,
                                                       reinterpret_cast<void**>(&dx12.frames[n].DynamicVB_mapped));

            memcpy(dx12.frames[n].DynamicVB_mapped, circle_verts, buf_size);
        }
    }

    // Execute the initialization commands.
    if FAILED(cmdlist->Close()) {
        RH_FATAL("Failed to close command list");
    }
    ID3D12CommandList* cmdsLists[] = { cmdlist.Get() };
    dx12.CommandQueue_Direct->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // set shader compile definitions:
    // #define BATCH_AMOUNT 1024
    D3D_SHADER_MACRO shader_defines[] = {
        { "BATCH_AMOUNT", MAX_OBJECTS_STR },
        {  NULL,           NULL}
    };

    RH_DEBUG("Shader defines: %d", _countof(shader_defines)-1);
    for (uint32 n = 0; n < _countof(shader_defines)-1; n++) {
        RH_DEBUG("  '%s' = '%s'", shader_defines[n].Name, shader_defines[n].Definition);
    }

    // compile shaders
    ComPtr<ID3DBlob> vs_bytecode;
    {
        ComPtr<ID3DBlob> vs_errors;
        if FAILED(D3DCompileFromFile(L"../src/Shaders/color.hlsl",
                                     shader_defines,
                                     NULL,
                                     "VS",
                                     "vs_5_1",
                                     D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
                                     0,
                                     &vs_bytecode,
                                     &vs_errors)) {
            RH_FATAL("Failed to compile vertex shader");
            RH_FATAL("Errors: %s", (char*)vs_errors->GetBufferPointer());
            return false;
        }
    }

    ComPtr<ID3DBlob> ps_bytecode;
    {
        ComPtr<ID3DBlob> ps_errors;
        if FAILED(D3DCompileFromFile(L"../src/Shaders/color.hlsl",
                                     shader_defines,
                                     NULL,
                                     "PS",
                                     "ps_5_1",
                                     D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
                                     0,
                                     &ps_bytecode,
                                     &ps_errors)) {
            RH_FATAL("Failed to compile pixel shader");
            RH_FATAL("Errors: %s", (char*)ps_errors->GetBufferPointer());
            return false;
        }
    }

    // Pipeline state object (PSO)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_standard = {};

        pso_standard.pRootSignature = dx12.RootSignature.Get();
        pso_standard.VS.pShaderBytecode = vs_bytecode->GetBufferPointer();
        pso_standard.VS.BytecodeLength  = vs_bytecode->GetBufferSize();
        pso_standard.PS.pShaderBytecode = ps_bytecode->GetBufferPointer();
        pso_standard.PS.BytecodeLength  = ps_bytecode->GetBufferSize();

        pso_standard.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso_standard.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

        pso_standard.SampleMask = UINT_MAX;
        pso_standard.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso_standard.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_standard.InputLayout.pInputElementDescs = vert_desc;
        pso_standard.InputLayout.NumElements = 3;
        pso_standard.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_standard.NumRenderTargets = 1;
        pso_standard.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_standard.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pso_standard.SampleDesc.Count = 1;
        pso_standard.SampleDesc.Quality = 0;

        // create the pso
        if FAILED(dx12.Device->CreateGraphicsPipelineState(&pso_standard, IID_PPV_ARGS(&dx12.PSO_Standard))) {
            RH_FATAL("Failed to create PSO");
            return false;
        }

        // create a slightly modifed PSO that supports blending
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_blend = pso_standard;
        pso_blend.BlendState.AlphaToCoverageEnable                 = FALSE;  // needs multisample to be enabled
        pso_blend.BlendState.IndependentBlendEnable                = FALSE; // if true:   each (of 8) RT can have different blending
        pso_blend.BlendState.RenderTarget[0].BlendEnable           = TRUE;
        pso_blend.BlendState.RenderTarget[0].LogicOpEnable         = FALSE;
        pso_blend.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
        pso_blend.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
        pso_blend.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
        pso_blend.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
        pso_blend.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_ZERO;
        pso_blend.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        pso_blend.BlendState.RenderTarget[0].LogicOp               = D3D12_LOGIC_OP_NOOP;
        pso_blend.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        if FAILED(dx12.Device->CreateGraphicsPipelineState(&pso_blend, IID_PPV_ARGS(&dx12.PSO_Blend))) {
            RH_FATAL("Failed to create Blend PSO");
            return false;
        }
    }

    // Wait until initialization is complete.
    FlushCommandQueue();
    // now that the commands have been executed, upload_buffer can be released 
    // (this happens at end of scope automatically)

#if 0
    // List supported display modes
    IDXGIOutput* output;
    {
        RH_INFO("------ Output Devices --------------------------");

        uint32 out_idx = 0;
        while (adapter->EnumOutputs(out_idx, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            RH_INFO("Output %d: '%ls'", out_idx, desc.DeviceName);


            uint32 count = 0;
            uint32 flags = 0;
            DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
            output->GetDisplayModeList(format, flags, &count, nullptr);

            std::vector<DXGI_MODE_DESC> modeList(count);
            output->GetDisplayModeList(format, flags, &count, &modeList[0]);

            for (auto& x : modeList) {
                uint32 n = x.RefreshRate.Numerator;
                uint32 d = x.RefreshRate.Denominator;

                //RH_INFO("  %u x %u @ %u/%u Hz", x.Width, x.Height, n, d);
            }

            ++out_idx;
        }
    }
#endif

    return true;
}

bool create_pipeline() {
    return true;
}

void kill_renderer() {
    // flush the command queues in case
    FlushCommandQueue();

    // unmap constant buffers
    for (uint32 n = 0; n < dx12.num_frames_in_flight; n++) {
        dx12.frames[n].upload_cbuffer_PerFrame->Unmap(0, nullptr);
        dx12.frames[n].upload_cbuffer_PerFrame_mapped = nullptr;

        dx12.frames[n].upload_cbuffer_PerModel->Unmap(0, nullptr);
        dx12.frames[n].upload_cbuffer_PerModel_mapped = nullptr;
    }
}

void d3d_debug_msg_callback(D3D12_MESSAGE_CATEGORY Category,
                            D3D12_MESSAGE_SEVERITY Severity,
                            D3D12_MESSAGE_ID ID,
                            LPCSTR pDescription,
                            void* pContext) {
    log_level level = LOG_LEVEL_TRACE;
    switch (Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: { level = log_level::LOG_LEVEL_FATAL; } break;
        case D3D12_MESSAGE_SEVERITY_ERROR:      { level = log_level::LOG_LEVEL_ERROR; } break;
        case D3D12_MESSAGE_SEVERITY_WARNING:    { level = log_level::LOG_LEVEL_WARN;  } break;
        case D3D12_MESSAGE_SEVERITY_INFO:       { level = log_level::LOG_LEVEL_INFO;  } break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE:    { level = log_level::LOG_LEVEL_DEBUG; } break;
    }

    char* msg_cat = "";
    switch (Category) {
        case D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED:    msg_cat = "DXAppDefined    "; break;
        case D3D12_MESSAGE_CATEGORY_MISCELLANEOUS:          msg_cat = "DXMisc          "; break;
        case D3D12_MESSAGE_CATEGORY_INITIALIZATION:         msg_cat = "DXInitialization"; break;
        case D3D12_MESSAGE_CATEGORY_CLEANUP:                msg_cat = "DXCleanup       "; break;
        case D3D12_MESSAGE_CATEGORY_COMPILATION:            msg_cat = "DXCompilation   "; break;
        case D3D12_MESSAGE_CATEGORY_STATE_CREATION:         msg_cat = "DXCreation      "; break;
        case D3D12_MESSAGE_CATEGORY_STATE_SETTING:          msg_cat = "DXSetting       "; break;
        case D3D12_MESSAGE_CATEGORY_STATE_GETTING:          msg_cat = "DXGetting       "; break;
        case D3D12_MESSAGE_CATEGORY_RESOURCE_MANIPULATION:  msg_cat = "DXManipulation  "; break;
        case D3D12_MESSAGE_CATEGORY_EXECUTION:              msg_cat = "DXExecution     "; break;
        case D3D12_MESSAGE_CATEGORY_SHADER:                 msg_cat = "DXShader        "; break;
    }

    LogOutput(level, "[%s] MsgId {%u} '%ls'", msg_cat, ID, pDescription);
}

bool renderer_begin_Frame() {
    //ImGui::Begin("Renderer");
    //ImGui::Text("delta_time: %.2f ms", delta_time*1000.0f);
    //ImGui::Text("frame_number: %d", backend->frame_number);
    //
    //if (!backend->begin_frame(delta_time)) {
    //    return false;
    //}
    //
    //backend->set_viewport(0, 0, render_state->render_width, render_state->render_height);
    ////backend->clear_viewport(0.8f, 0.1f, 0.8f, 0.1f);
    ////backend->clear_viewport(0.112f, 0.112f, 0.069f, 0.0f);
    //backend->clear_viewport(0.0f, 0.0f, 0.0f, 0.0f);

    return true;
}
bool renderer_end_Frame() {
    //ImGui::End(); // ImGui::Begin("Renderer");
    //renderer_debug_UI_end_frame(draw_ui);
    //
    //bool32 result = backend->end_frame(delta_time);
    //backend->frame_number++;
    //return result;

    return true;
}

bool renderer_draw_frame() {
    if (renderer_begin_Frame()) {
        // keep track of time (poorly cx)
        static real32 t = 0.0f;
        t += 1.0f / 60.0f;

        // wait for new frame to be done on the GPU
        uint64 completed_value = dx12.Fence->GetCompletedValue();
        if (dx12.frames[dx12.frame_idx].FenceValue != 0 && dx12.Fence->GetCompletedValue() < dx12.frames[dx12.frame_idx].FenceValue) {
            HANDLE event_handle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
            dx12.Fence->SetEventOnCompletion(dx12.frames[dx12.frame_idx].FenceValue, event_handle);
            ::WaitForSingleObject(event_handle, INFINITE);
            ::CloseHandle(event_handle);
        }

        laml::Mat4 eye(1.0f);

        // upload the constant buffer - PerFrame
        {
            cbLayout_PerFrame cbdata;

            float AR = (float)render.render_width / (float)render.render_height;
            laml::transform::create_projection_perspective(cbdata.r_Projection, 60.0f, AR, 0.01f, 100.0f);

            laml::Mat4 cam_trans;
            laml::transform::create_transform_translate(cam_trans, laml::Vec3(0.0f, 0.0f, 2.0f));
            laml::transform::create_view_matrix_from_transform(cbdata.r_View, cam_trans);
            
            size_t cbsize = sizeof(cbdata);
            memcpy(dx12.frames[dx12.frame_idx].upload_cbuffer_PerFrame_mapped, &cbdata, cbsize);
        }

        // upload the constant buffer - PerModel
        {
            cbLayout_PerModel cbdata;
            //laml::transform::create_transform_rotation(cbdata.r_Model[0], t *  60.0f, 0.0f, 0.0f);
            laml::transform::create_transform(cbdata.r_Model[0], t*60.0f, 0.0f, 0.0f, laml::Vec3(0.0f, 0.0f, 0.5f));
            //laml::transform::create_transform_rotation(cbdata.r_Model[1], t * 120.0f, 0.0f, 0.0f);
            laml::transform::create_transform(cbdata.r_Model[1], t*120.0f, 0.0f, 0.0f, laml::Vec3(0.0f, 0.0f, 1.0f));
            size_t cbsize = sizeof(cbdata.r_Model[0]) * 2; // only the first 2 change
            memcpy(dx12.frames[dx12.frame_idx].upload_cbuffer_PerModel_mapped, &cbdata, cbsize);

            laml::transform::create_transform_translate(cbdata.r_Model[3], laml::Vec3(0.0f, 0.0f, 1.5f));
            size_t offset = sizeof(cbdata.r_Model[0]) * 3;
            cbsize = sizeof(cbdata.r_Model[0]) * 1; // update 4
            memcpy(dx12.frames[dx12.frame_idx].upload_cbuffer_PerModel_mapped + offset, ((BYTE*)(&cbdata)) + offset, cbsize);
        }

        // update dynamic vertex buffer
        {
            // generate vertices for a circle
            Vertex circle_verts[num_circle_verts * 3];

            generate_circle_verts(circle_verts, num_circle_verts, t);
            const UINT buf_size = sizeof(circle_verts);

            memcpy(dx12.frames[dx12.frame_idx].DynamicVB_mapped, circle_verts, buf_size);
        }

        // Start recording commands
        auto allocator  = dx12.frames[dx12.frame_idx].CommandAllocator;
        auto backbuffer = dx12.frames[dx12.frame_idx].BackBuffer;

        allocator->Reset();
        dx12.CmdList_Direct->Reset(allocator.Get(), dx12.PSO_Standard.Get());

        // set viewport
        D3D12_VIEWPORT viewport;
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width    = static_cast<float>(render.render_width);
        viewport.Height   = static_cast<float>(render.render_height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        dx12.CmdList_Direct->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = { 0, 0, (LONG)render.render_width, (LONG)render.render_height };
        dx12.CmdList_Direct->RSSetScissorRects(1, &scissor);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(dx12.RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                          dx12.frame_idx, dx12.RTVDescriptorSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dx12.DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        // clear render target
        {
            // transition the backbuffer into render target
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                backbuffer.Get(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

            dx12.CmdList_Direct->ResourceBarrier(1, &barrier);

            // clear the color buffer
            FLOAT color[] = { 0.4f, 0.6f, 0.9f, 1.0f }; // cornflower blue
            dx12.CmdList_Direct->ClearRenderTargetView(rtv, color, 0, nullptr);

            // clear the depth/stencil buffer
            dx12.CmdList_Direct->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

            // specify render target to use
            dx12.CmdList_Direct->OMSetRenderTargets(1, &rtv, true, &dsv);
        }

        ID3D12DescriptorHeap* descriptorHeaps[] = {
            dx12.CBVDescriptorHeap.Get(),
            dx12.SamDescriptorHeap.Get(),
        };
        dx12.CmdList_Direct->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
        dx12.CmdList_Direct->SetGraphicsRootSignature(dx12.RootSignature.Get());

        float alpha = 1.0f;
        UINT* pAlpha = (UINT*)(&alpha);

        float tex_scale = 1.0f;
        UINT* pTexScale = (UINT*)(&tex_scale);

        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 0, 0);
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 1, 1);
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, *pAlpha, 2);
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, *pTexScale, 3);
        dx12.CmdList_Direct->SetGraphicsRootConstantBufferView(1, dx12.frames[dx12.frame_idx].upload_cbuffer_PerFrame->GetGPUVirtualAddress());
        dx12.CmdList_Direct->SetGraphicsRootConstantBufferView(2, dx12.frames[dx12.frame_idx].upload_cbuffer_PerModel->GetGPUVirtualAddress());

        CD3DX12_GPU_DESCRIPTOR_HANDLE tex(dx12.CBVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        tex.Offset(0, dx12.CBVDescriptorSize);
        dx12.CmdList_Direct->SetGraphicsRootDescriptorTable(3, tex);

        CD3DX12_GPU_DESCRIPTOR_HANDLE sam(dx12.SamDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        //sam.Offset(0, dx12.CBVDescriptorSize);
        dx12.CmdList_Direct->SetGraphicsRootDescriptorTable(4, sam);

        dx12.CmdList_Direct->IASetVertexBuffers(0, 1, &dx12.TriangleVBView);
        dx12.CmdList_Direct->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Bind as shader input to root parameter.
        //CD3DX12_GPU_DESCRIPTOR_HANDLE tex = …;
        //dx12.CommandList->SetGraphicsRootDescriptorTable(rootParamIndex, tex);

        // bind and draw the vertex buffer
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 0, 0);
        //dx12.CommandList->DrawInstanced(3, 1, 0, 0);

        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 1, 0);
        //dx12.CommandList->DrawInstanced(3, 1, 0, 0);

        // set new vertex buffer
        dx12.CmdList_Direct->IASetVertexBuffers(0, 1, &dx12.frames[dx12.frame_idx].DynamicVBView);

        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 2, 0);
        dx12.CmdList_Direct->DrawInstanced(num_circle_verts * 3, 1, 0, 0);

        // draw the triangle, transparent
        dx12.CmdList_Direct->SetPipelineState(dx12.PSO_Blend.Get());
        dx12.CmdList_Direct->IASetVertexBuffers(0, 1, &dx12.TriangleVBView);
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 0, 0); // batch_idx
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 0, 1); // use texture for diffuse
        alpha = 0.5f;
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, *pAlpha, 2);
        dx12.CmdList_Direct->DrawInstanced(3, 1, 0, 0);

        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 1, 0); // batch_idx
        dx12.CmdList_Direct->DrawInstanced(3, 1, 0, 0);

        // draw chainlink fence over everything
        // Chainlink texture has an alpha channel
        // instead of alpha-blending, we discard using clip()
        dx12.CmdList_Direct->SetPipelineState(dx12.PSO_Standard.Get());
        dx12.CmdList_Direct->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        dx12.CmdList_Direct->IASetVertexBuffers(0, 1, &dx12.ScreenVBView);
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 3, 0); // batch_idx
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, 1, 1); // use texture for diffuse
        alpha = 1.0f;
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, *pAlpha, 2);
        tex_scale = 1.0f;
        dx12.CmdList_Direct->SetGraphicsRoot32BitConstant(0, *pTexScale, 3);
        tex.Offset(1, dx12.CBVDescriptorSize);
        dx12.CmdList_Direct->SetGraphicsRootDescriptorTable(3, tex);

        dx12.CmdList_Direct->DrawInstanced(4, 1, 0, 0);

        //renderer_end_Frame();

        {
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                backbuffer.Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
            dx12.CmdList_Direct->ResourceBarrier(1, &barrier);

            if FAILED(dx12.CmdList_Direct->Close()) {
                RH_FATAL("Could not close command list.");
                return false;
            }

            ID3D12CommandList* const commandLists[] = {
                dx12.CmdList_Direct.Get()
            };
            dx12.CommandQueue_Direct->ExecuteCommandLists(_countof(commandLists), commandLists);
        }
    }

    return false;
}

bool renderer_present(uint32 sync_interval) {
    const uint32 presentFlags = 0;
    if FAILED(dx12.SwapChain->Present(sync_interval, presentFlags)) {
        RH_FATAL("Error presenting.");
        return false;
    }

    // signal fence value
    dx12.frames[dx12.frame_idx].FenceValue = ++dx12.CurrentFence;
    dx12.CommandQueue_Direct->Signal(dx12.Fence.Get(), dx12.frames[dx12.frame_idx].FenceValue);

    dx12.frame_idx = dx12.SwapChain->GetCurrentBackBufferIndex();

    return true;
};