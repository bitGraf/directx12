#include "Renderer.h"

#include "Platform/platform.h"
#include "Core/Asserts.h"

// DirectX 12 headers.
#include <directx/d3d12.h>
#include <directx/d3dx12.h>
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
    uint32                                              DSVDescriptorSize;
    uint32                                              CBVDescriptorSize;

    uint32                                              CurrentBackBufferIndex;

    Microsoft::WRL::ComPtr<ID3D12Fence> Fence;
    uint64 FenceValue = 0;
    uint64 FrameFenceValues[DX12State::NumFramesInFlight] = {};
    HANDLE FenceEvent;

    Microsoft::WRL::ComPtr<ID3D12Resource> DepthStencilBuffer;

    DWORD callback_cookie;
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
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

        if FAILED(dx12.Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dx12.RTVDescriptorHeap))) {
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
    }

    // create fence
    if FAILED(dx12.Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx12.Fence))) {
        RH_FATAL("Could not create fence!");
        return false;
    }

    dx12.FenceEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    AssertMsg(dx12.FenceEvent, "Failed to create fence event.");


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
        dx12.CommandList->Reset(allocator.Get(), nullptr);

        // clear render target
        {
            CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                backbuffer.Get(),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

            dx12.CommandList->ResourceBarrier(1, &barrier);

            FLOAT color[] = { 0.4f, 0.6f, 0.9f, 1.0f }; // cornflower blue
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(dx12.RTVDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                                              dx12.CurrentBackBufferIndex, dx12.RTVDescriptorSize);

            dx12.CommandList->ClearRenderTargetView(rtv, color, 0, nullptr);
        }

        return renderer_end_Frame();
    }

    return false;
}