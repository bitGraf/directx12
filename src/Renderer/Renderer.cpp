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

#if defined(min)
#undef min
#endif


#if defined(max)
#undef max
#endif

#include <vector>
#include <chrono>

#include "Core/Logger.h"

struct cbLayout_PerFrame {
    DirectX::XMFLOAT4X4 r_Projection = DirectX::XMFLOAT4X4(1.0f, 0.0f, 0.0f, 0.0f,
                                                           0.0f, 1.0f, 0.0f, 0.0f,
                                                           0.0f, 0.0f, 1.0f, 0.0f,
                                                           0.0f, 0.0f, 0.0f, 1.0f);
    DirectX::XMFLOAT4X4 r_View       = DirectX::XMFLOAT4X4(1.0f, 0.0f, 0.0f, 0.0f,
                                                           0.0f, 1.0f, 0.0f, 0.0f,
                                                           0.0f, 0.0f, 1.0f, 0.0f,
                                                           0.0f, 0.0f, 0.0f, 1.0f);
};
struct cbLayout_PerModel {
    DirectX::XMFLOAT4X4 r_Model = DirectX::XMFLOAT4X4(1.0f, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 1.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 1.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 1.0f);
};

struct DX12State {
    // DirectX 12 Objects
    static const uint8                                  NumFramesInFlight = 3;
    Microsoft::WRL::ComPtr<ID3D12Device2>               Device;
    Microsoft::WRL::ComPtr<ID3D12Debug>                 DebugController;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>          CommandQueue_Direct;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>          CommandQueue_Copy;
    Microsoft::WRL::ComPtr<IDXGISwapChain4>             SwapChain;
    Microsoft::WRL::ComPtr<ID3D12Resource>              BackBuffers[DX12State::NumFramesInFlight];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   CommandList;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      CommandAllocators[DX12State::NumFramesInFlight];

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        RTVDescriptorHeap;
    uint32                                              RTVDescriptorSize;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        DSVDescriptorHeap;
    uint32                                              DSVDescriptorSize;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>        CBVDescriptorHeap;
    uint32                                              CBVDescriptorSize;

    uint32                                              CurrentBackBufferIndex;

    Microsoft::WRL::ComPtr<ID3D12Fence> Fence;
    uint64 FenceValue = 0;
    uint64 FrameFenceValues[DX12State::NumFramesInFlight] = {};
    HANDLE FenceEvent;

    Microsoft::WRL::ComPtr<ID3D12Resource> DepthStencilBuffer;

    DWORD callback_cookie;

    // mesh and data
    Microsoft::WRL::ComPtr<ID3D12Resource> vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view;

    // constant buffer
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_cbuffer_PerFrame;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_cbuffer_PerModel;

    // Pipeline State Object
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
};
global_variable DX12State dx12;

struct RenderState {
    uint32 render_width  = 800;
    uint32 render_height = 600;
};
global_variable RenderState render;

// callbacks
void d3d_debug_msg_callback(D3D12_MESSAGE_CATEGORY Category, 
                            D3D12_MESSAGE_SEVERITY Severity, 
                            D3D12_MESSAGE_ID ID, 
                            LPCSTR pDescription, 
                            void* pContext);

void FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
    dx12.FenceValue++;

    // Add an instruction to the command queue to set a new fence point.  Because we 
    // are on the GPU timeline, the new fence point won't be set until the GPU finishes
    // processing all the commands prior to this Signal().
    if FAILED(dx12.CommandQueue_Direct->Signal(dx12.Fence.Get(), dx12.FenceValue)) {
        RH_FATAL("Failed to signal");
    }

	// Wait until the GPU has completed commands up to this fence point.
    if(dx12.Fence->GetCompletedValue() < dx12.FenceValue)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

        // Fire event when GPU hits current fence.  
        if FAILED(dx12.Fence->SetEventOnCompletion(dx12.FenceValue, eventHandle)) {
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

            RH_INFO("Device %d: %ls", i, desc.Description);
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
        RH_INFO("Chosen Device: [%d] '%ls', %.1llf GB", max_idx, desc.Description, ((double)desc.DedicatedVideoMemory) / (1024.0*1024.0*1024.0));
    }

    // create Device
    {
        if FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dx12.Device))) {
            RH_FATAL("Failed to create Device!");
            return false;
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
        queue_desc.Type     = D3D12_COMMAND_LIST_TYPE_COPY;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_desc.NodeMask = 0;

        if (FAILED(dx12.Device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&dx12.CommandQueue_Copy)))) {
            RH_FATAL("Could not create copy queue");
            return false;
        }

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
        swapChainDesc.BufferCount   = dx12.NumFramesInFlight;
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

        dx12.CurrentBackBufferIndex = dx12.SwapChain->GetCurrentBackBufferIndex();
    }

    // Query Descriptor Heap Sizes
    dx12.RTVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dx12.DSVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    dx12.CBVDescriptorSize = dx12.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Create Descriptor Heap
    { // RTV
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = dx12.NumFramesInFlight;
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.RTVDescriptorHeap))) {
            RH_FATAL("Failed making descriptor heap");
            return false;
        }
    }
    { // CBV
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.NumDescriptors = 1;
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

    // setup back buffers
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(dx12.RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        for (int i = 0; i < dx12.NumFramesInFlight; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            if FAILED(dx12.SwapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer))) {
                RH_FATAL("Failed getting backbuffer %d", i);
                return false;
            }

            dx12.Device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtvHandle);

            dx12.BackBuffers[i] = backBuffer;

            rtvHandle.Offset(dx12.RTVDescriptorSize);
        }
    }

    // command allocators - one per frame
    for (uint8 n = 0; n < dx12.NumFramesInFlight; n++) {
        if (FAILED(dx12.Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dx12.CommandAllocators[n])))) {
            RH_FATAL("Could not create command allocators");
            return false;
        }
    }

    // create command list for current backbuffer
    {
        if FAILED(dx12.Device->CreateCommandList(0, 
                                            D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                            dx12.CommandAllocators[dx12.CurrentBackBufferIndex].Get(), 
                                            nullptr, 
                                            IID_PPV_ARGS(dx12.CommandList.GetAddressOf()))) {
            RH_FATAL("Could not create command list!");
            return false;
        }

        // start it closed, since each update cycle starts with reset, and it needs to be closed.
        dx12.CommandList->Close();
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

    dx12.FenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    AssertMsg(dx12.FenceEvent, "Failed to create fence event.");

    // create constant buffer - PerFrame
    {
        uint32 cbSize = sizeof(cbLayout_PerFrame);
        cbSize = (cbSize + 255) & ~255; // make it a multiple of 256, minimum alloc size

        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize * 1);
        dx12.Device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
                                             &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&dx12.upload_cbuffer_PerFrame));

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
        cbv_desc.BufferLocation = dx12.upload_cbuffer_PerFrame->GetGPUVirtualAddress();
        cbv_desc.SizeInBytes    = cbSize;
        dx12.Device->CreateConstantBufferView(&cbv_desc, 
                                              dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    }
    // create constant buffer - PerModel
    {
        uint32 cbSize = sizeof(cbLayout_PerModel);
        cbSize = (cbSize + 255) & ~255; // make it a multiple of 256, minimum alloc size

        auto prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(cbSize * 1);
        dx12.Device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_NONE,
                                             &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                             nullptr, IID_PPV_ARGS(&dx12.upload_cbuffer_PerModel));

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
        cbv_desc.BufferLocation = dx12.upload_cbuffer_PerModel->GetGPUVirtualAddress();
        cbv_desc.SizeInBytes    = cbSize;
        dx12.Device->CreateConstantBufferView(&cbv_desc, 
                                              dx12.CBVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // create root signature
    {
        D3D12_ROOT_PARAMETER root_parameters[2];
        
        root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_parameters[0].Descriptor.ShaderRegister = 0;
        root_parameters[0].Descriptor.RegisterSpace = 0;

        root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        root_parameters[1].Descriptor.ShaderRegister = 1;
        root_parameters[1].Descriptor.RegisterSpace = 0;

        CD3DX12_ROOT_SIGNATURE_DESC root_sig_desc(2, root_parameters, 0, nullptr,
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
                                                   IID_PPV_ARGS(&dx12.root_signature))) {
            RH_FATAL("Could not create root signature");
            return false;
        }
    }

    // load mesh data
    auto alloc = dx12.CommandAllocators[dx12.CurrentBackBufferIndex];
    auto cmdlist = dx12.CommandList;

    alloc->Reset();
    cmdlist->Reset(alloc.Get(), nullptr);
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buffer;
    D3D12_INPUT_ELEMENT_DESC vert_desc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    {
        struct Vertex
        {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT4 color;
        };

        // Define the geometry for a triangle.
        Vertex vertex_data[] =
        {
            { {  0.0f,   0.25f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
            { {  0.25f, -0.25f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
            { { -0.25f, -0.25f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } }
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
            nullptr, IID_PPV_ARGS(&dx12.vertex_buffer));

        // 2. create a temp upload buffer in the upload heap
        prop = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        desc = CD3DX12_RESOURCE_DESC::Buffer(buf_size);
        dx12.Device->CreateCommittedResource(
            &prop, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, 
            nullptr, IID_PPV_ARGS(&upload_buffer));

        // 3. describe the data we wish to copy to the gpu
        D3D12_SUBRESOURCE_DATA subResourceData = {};
        subResourceData.pData      = vertex_data;
        subResourceData.RowPitch   = buf_size;
        subResourceData.SlicePitch = subResourceData.RowPitch;

        // 4. schedule the copy into the default resource
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.vertex_buffer.Get(), 
                                                                              D3D12_RESOURCE_STATE_COMMON, 
                                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        cmdlist->ResourceBarrier(1, &barrier);
        UpdateSubresources<1>(cmdlist.Get(), dx12.vertex_buffer.Get(), upload_buffer.Get(), 0, 0, 1, &subResourceData);
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(dx12.vertex_buffer.Get(),
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ);
        cmdlist->ResourceBarrier(1, &barrier);

        // initialize the vertex buffer view.
        dx12.vertex_buffer_view.BufferLocation = dx12.vertex_buffer->GetGPUVirtualAddress();
        dx12.vertex_buffer_view.StrideInBytes  = sizeof(Vertex);
        dx12.vertex_buffer_view.SizeInBytes    = buf_size;

        // Transition the resource from its initial state to be used as a depth buffer.
        auto t = CD3DX12_RESOURCE_BARRIER::Transition(dx12.DepthStencilBuffer.Get(),
                                                      D3D12_RESOURCE_STATE_COMMON, 
                                                      D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmdlist->ResourceBarrier(1, &t);
    }

    // Execute the initialization commands.
    if FAILED(cmdlist->Close()) {
        RH_FATAL("Failed to close command list");
    }
    ID3D12CommandList* cmdsLists[] = { cmdlist.Get() };
    dx12.CommandQueue_Direct->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // compile shaders
    ComPtr<ID3DBlob> vs_bytecode;
    {
        ComPtr<ID3DBlob> vs_errors;
        if FAILED(D3DCompileFromFile(L"../src/Shaders/color.hlsl",
                                     NULL,
                                     NULL,
                                     "VS",
                                     "vs_5_0",
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
                                     NULL,
                                     NULL,
                                     "PS",
                                     "ps_5_0",
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
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = dx12.root_signature.Get();

        desc.VS.pShaderBytecode = vs_bytecode->GetBufferPointer();
        desc.VS.BytecodeLength  = vs_bytecode->GetBufferSize();

        desc.PS.pShaderBytecode = ps_bytecode->GetBufferPointer();
        desc.PS.BytecodeLength  = ps_bytecode->GetBufferSize();

        desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);

        desc.SampleMask = UINT_MAX;

        desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

        desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

        desc.InputLayout.pInputElementDescs = vert_desc;
        desc.InputLayout.NumElements = 2;

        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        desc.NumRenderTargets = 1;
        
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

        desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;

        // create the pso
        if FAILED(dx12.Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&dx12.pso))) {
            RH_FATAL("Failed to create PSO");
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

bool renderer_present() {
    auto backbuffer = dx12.BackBuffers[dx12.CurrentBackBufferIndex];
    {
        CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            backbuffer.Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        dx12.CommandList->ResourceBarrier(1, &barrier);

        if FAILED(dx12.CommandList->Close()) {
            RH_FATAL("Could not close command list.");
            return false;
        }

        ID3D12CommandList* const commandLists[] = {
            dx12.CommandList.Get()
        };
        dx12.CommandQueue_Direct->ExecuteCommandLists(_countof(commandLists), commandLists);

        uint32 syncInterval = 0;
        uint32 presentFlags = 0;
        if FAILED(dx12.SwapChain->Present(syncInterval, presentFlags)) {
            RH_FATAL("Error presenting.");
            return false;
        }

        // signal fence value
        dx12.FenceValue++;
        dx12.CommandQueue_Direct->Signal(dx12.Fence.Get(), dx12.FenceValue);
        dx12.FrameFenceValues[dx12.CurrentBackBufferIndex] = dx12.FenceValue;

        dx12.CurrentBackBufferIndex = dx12.SwapChain->GetCurrentBackBufferIndex();

        // WaitForFenceValue
        if (dx12.Fence->GetCompletedValue() < dx12.FrameFenceValues[dx12.CurrentBackBufferIndex]) {
            dx12.Fence->SetEventOnCompletion(dx12.FrameFenceValues[dx12.CurrentBackBufferIndex], dx12.FenceEvent);

            std::chrono::milliseconds duration = std::chrono::milliseconds::max();
            ::WaitForSingleObject(dx12.FenceEvent, static_cast<DWORD>(duration.count()));
        }
    }

    return true;
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

        auto allocator = dx12.CommandAllocators[dx12.CurrentBackBufferIndex];
        auto backbuffer = dx12.BackBuffers[dx12.CurrentBackBufferIndex];

        allocator->Reset();
        dx12.CommandList->Reset(allocator.Get(), dx12.pso.Get());

        // set viewport
        D3D12_VIEWPORT viewport;
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width    = static_cast<float>(800);
        viewport.Height   = static_cast<float>(600);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        dx12.CommandList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = { 0, 0, 800, 600 };
        dx12.CommandList->RSSetScissorRects(1, &scissor);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(dx12.RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                          dx12.CurrentBackBufferIndex, dx12.RTVDescriptorSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dx12.DSVDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

        // clear render target
        {
            // transition the backbuffer into render target
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                backbuffer.Get(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

            dx12.CommandList->ResourceBarrier(1, &barrier);

            // clear the color buffer
            FLOAT color[] = { 0.4f, 0.6f, 0.9f, 1.0f }; // cornflower blue
            dx12.CommandList->ClearRenderTargetView(rtv, color, 0, nullptr);

            // clear the depth/stencil buffer
            dx12.CommandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

            // specify render target to use
            dx12.CommandList->OMSetRenderTargets(1, &rtv, true, &dsv);
        }

        DirectX::XMFLOAT4X4 eye(1.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 1.0f);
        // upload the constant buffer - PerFrame
        {
            cbLayout_PerFrame cbdata;
            cbdata.r_Projection = eye;
            cbdata.r_View       = eye;
            size_t cbsize = sizeof(cbdata);

            BYTE* mapped_data = nullptr;
            dx12.upload_cbuffer_PerFrame->Map(0, nullptr, reinterpret_cast<void**>(&mapped_data));
            memcpy(mapped_data, &cbdata, cbsize);
            dx12.upload_cbuffer_PerFrame->Unmap(0, nullptr);
        }

        // upload the constant buffer - PerModel
        {
            cbLayout_PerModel cbdata;
            cbdata.r_Model = eye;
            size_t cbsize = sizeof(cbdata);

            BYTE* mapped_data = nullptr;
            dx12.upload_cbuffer_PerModel->Map(0, nullptr, reinterpret_cast<void**>(&mapped_data));
            memcpy(mapped_data, &cbdata, cbsize);
            dx12.upload_cbuffer_PerModel->Unmap(0, nullptr);
        }

        dx12.CommandList->SetGraphicsRootSignature(dx12.root_signature.Get());
        //ID3D12DescriptorHeap* descriptorHeaps[] = {
        //    dx12.CBVDescriptorHeap.Get()
        //};
        //dx12.CommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

        //CD3DX12_GPU_DESCRIPTOR_HANDLE cbv(dx12.CBVDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        //cbv.Offset(0, dx12.CBVDescriptorSize);

        //dx12.CommandList->SetGraphicsRootDescriptorTable(0, cbv);
        dx12.CommandList->SetGraphicsRootConstantBufferView(0, dx12.upload_cbuffer_PerFrame->GetGPUVirtualAddress());
        dx12.CommandList->SetGraphicsRootConstantBufferView(1, dx12.upload_cbuffer_PerModel->GetGPUVirtualAddress());

        // bind and draw the vertex buffer
        dx12.CommandList->IASetVertexBuffers(0, 1, &dx12.vertex_buffer_view);
        dx12.CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dx12.CommandList->DrawInstanced(3, 1, 0, 0);

        return renderer_end_Frame();
    }

    return false;
}