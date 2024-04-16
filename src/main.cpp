#include <stdio.h>

#include "Memory/Memory_Arena.h"
#include "Platform/Platform.h"
#include "Core/Application.h"
#include "Core/Logger.h"
#include "Core/Event.h"
#include "Core/Input.h"
#include "Renderer/Renderer.h"

#include <laml/laml.hpp>

struct RohinEngine {
    real32 target_frame_time;
    real32 last_frame_time;
    bool32 is_running;
    bool32 is_paused;
    bool32 lock_framerate;

    uint8* engine_memory;
    uint64 engine_memory_size;
    memory_arena engine_arena;
    memory_arena resource_arena;
    memory_arena frame_render_arena;

    bool32 debug_mode;

    // tmp, should probably be pulled out into a 'scene' representation
    real32 view_vert_fov;
    laml::Mat4 projection_matrix;

    RohinMemory app_memory;
};
global_variable RohinEngine engine;

bool32 engine_on_event(uint16 code, void* sender, void* listener, event_context context);
bool32 engine_on_resize(uint16 code, void* sender, void* listener, event_context context);

//int main() {
int WinMain() {
    printf("Hello, World!\n");
    InitLogging(true, log_level::LOG_LEVEL_TRACE);
    platform_setup_paths();

    AppConfig config;
    config.application_name = "DX12 Test";
    config.start_x = 20;
    config.start_y = 20;
    config.start_width = 800;
    config.start_height = 600;
    config.requested_memory = 1024*1024;
    if (!platform_startup(&config)) {
        RH_FATAL("Failed on platform startup!");
        return false;
    }

    RH_INFO("Rohin Engine v0.0.1");

#if RH_INTERNAL
    uint64 base_address = Terabytes(2);
#else
    uint64 base_address = 0;
#endif

    engine.debug_mode = false;
    engine.engine_memory_size = Megabytes(64);
    engine.engine_memory = (uint8*)platform_alloc(engine.engine_memory_size, 0);
    CreateArena(&engine.frame_render_arena, Megabytes(16), engine.engine_memory);
    CreateArena(&engine.engine_arena,       Megabytes(16), engine.engine_memory + Megabytes(16));
    CreateArena(&engine.resource_arena,     Megabytes(32), engine.engine_memory + Megabytes(32));

    uint32 monitor_refresh_hz = 60;
    uint32 target_framerate = 60;

    // renderer startup
    if (!init_renderer()) {
        RH_FATAL("Failed to initialize Renderer!\n");
        return -1;
    }
    ////////////////////////////////////////////////////////////////////////////////////////

    // init resources
    ////////////////////////////////////////////////////////////////////////////////////////

    // after resource system is setup
    create_pipeline();

    engine.target_frame_time = 1.0f / ((real32)target_framerate);
    engine.last_frame_time = engine.target_frame_time;
    engine.lock_framerate = true;

    // Initialize some systems
    event_init(&engine.engine_arena);
    event_register(EVENT_CODE_APPLICATION_QUIT, 0, engine_on_event);
    event_register(EVENT_CODE_KEY_PRESSED, 0, engine_on_event);
    event_register(EVENT_CODE_RESIZED, 0, engine_on_resize);

    input_init(&engine.engine_arena);

    void* memory = platform_alloc(config.requested_memory, base_address);
    if (memory) {
        engine.app_memory.AppStorage      = memory;
        engine.app_memory.AppStorageSize  = Megabytes(4);
        engine.app_memory.GameStorage     = ((uint8*)memory + engine.app_memory.AppStorageSize);
        engine.app_memory.GameStorageSize = Megabytes(4);

        engine.is_running = true;

        ////////////////////////////////////////////////////////////////////////////////////////
        // app startup

        uint64 LastCounter = platform_get_wall_clock();
        uint64 FlipWallClock = platform_get_wall_clock();
        //uint64 LastCycleCount = __rdtsc();

        real32 AR = (real32)config.start_width / (real32)config.start_height;
        engine.view_vert_fov = 75.0f;
        laml::transform::create_projection_perspective(engine.projection_matrix, engine.view_vert_fov, AR, 0.1f, 100.0f);

        ////////////////////////////////////////////////////////////////////////////////////////
        // app initialize

        // Game Loop!
        RH_INFO("------ Starting Main Loop ----------------------");
        while(engine.is_running) {
            if (!platform_process_messages()) {
                engine.is_running = false;
            }

            if (!engine.is_paused) {
                // app update

                // render scene
                renderer_draw_frame();

                uint64 WorkCounter = platform_get_wall_clock();
                real32 WorkSecondsElapsed = (real32)platform_get_seconds_elapsed(LastCounter, WorkCounter);

                // TODO: Untested! buggy
                real32 SecondsElapsedForFrame = WorkSecondsElapsed;
                bool32 LockFramerate = false;
                if (LockFramerate) {
                    if (SecondsElapsedForFrame < engine.target_frame_time) {
                        uint64 SleepMS = (uint64)(1000.0f*(engine.target_frame_time - SecondsElapsedForFrame));
                        if (SleepMS > 0) {
                            platform_sleep(SleepMS);
                        }
    
                        while (SecondsElapsedForFrame < engine.target_frame_time) {
                            SecondsElapsedForFrame = (real32)platform_get_seconds_elapsed(LastCounter, platform_get_wall_clock());
                        }
                    } else {
                        // TODO: Missed Frame Rate!
                        //RH_ERROR("Frame rate missed!");
                    }
                }

                uint64 EndCounter = platform_get_wall_clock();
                engine.last_frame_time = (real32)platform_get_seconds_elapsed(LastCounter, EndCounter);
                real32 MSPerFrame = (real32)(1000.0f * engine.last_frame_time);
                //MSLastFrame.addSample(MSPerFrame);
                LastCounter = EndCounter;
    
                //Win32DisplayBufferToWindow(DeviceContext, Dimension.Width, Dimension.Height);
                //platform_swap_buffers();
                //renderer_present();
    
                FlipWallClock = platform_get_wall_clock();

                //uint64 EndCycleCount = __rdtsc();
                //uint64 CyclesElapsed = EndCycleCount - LastCycleCount;
                //LastCycleCount = EndCycleCount;
    
                real32 FPS = 1000.0f / MSPerFrame;
                //real32 MCPF = ((real32)CyclesElapsed / (1000.0f*1000.0f));
    
                #if 0
                RH_TRACE("Frame: %.02f ms  %.02ffps\n", MSPerFrame, FPS);
                #else
                platform_console_set_title("%s: %.02f ms, FPS: %.02ffps", config.application_name, MSPerFrame, FPS);
                #endif

                input_update(engine.last_frame_time);
            }
        }

        // app shutdown

        platform_free(memory);
    } else {
        RH_FATAL("Requested %u bytes of memory, but could not get it from the os!", config.requested_memory);
    }

    // renderer shutdown
    kill_renderer();
    platform_shutdown();

    // shutdown all systems
    input_shutdown();
    event_shutdown();
    ShutdownLogging();

    return 0;
}

bool32 engine_on_event(uint16 code, void* sender, void* listener, event_context context) {
    switch (code) {
        case EVENT_CODE_APPLICATION_QUIT:
            engine.is_running = false;
            return true;
        case EVENT_CODE_KEY_PRESSED:
            if (context.u16[0] == KEY_ESCAPE) {
                event_context no_data = {};
                event_fire(EVENT_CODE_APPLICATION_QUIT, 0, no_data);
            } else if (context.u16[0] == KEY_F1) {
                engine.debug_mode = !engine.debug_mode;
                RH_INFO("Debug Mode: %s", engine.debug_mode ? "Enabled" : "Disabled");
            } else {
                keyboard_keys key = (keyboard_keys)context.u16[0];
                //RH_INFO("Key Pressed: [%s]", input_get_key_string(key));
            }
    }

    if ((code != EVENT_CODE_KEY_PRESSED)) {
        RH_TRACE("Engine[0x%016llX] recieved event code %d \n         "
                 "Sender=[0x%016llX] \n         "
                 "Listener=[0x%016llX] \n         "
                 "Data=[%llu], [%u,%u], [%hu,%hu,%hu,%hu]",
                 (uintptr_t)(&engine), code, (uintptr_t)sender, (uintptr_t)listener,
                 context.u64,
                 context.u32[0], context.u32[1],
                 context.u16[0], context.u16[1], context.u16[2], context.u16[3]);
    }

    //renderer_on_event(code, sender, listener, context);

    return false;
}

bool32 engine_on_resize(uint16 code, void* sender, void* listener, event_context context) {
    real32 width  = (real32)context.u32[0];
    real32 height = (real32)context.u32[1];
    laml::transform::create_projection_perspective(engine.projection_matrix, engine.view_vert_fov, width/height, 0.1f, 100.0f);

    //renderer_resized(context.u32[0], context.u32[1]);
    //engine.app->on_resize(engine.app, context.u32[0], context.u32[1]);

    return false;
}