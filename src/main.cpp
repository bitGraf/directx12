#if 0
#include <stdio.h>

#include "Memory/Memory_Arena.h"
#include "Memory/Memory.h"
#include "Core/Asserts.h"
#include "Core/Logger.h"

typedef uint16 Texture_Handle;

struct Renderer_Texture {
    uint64 gpu_handle;
    uint16 width, height;
    uint16 format;

    char*    name;
    wchar_t* filename;
    uint8*   data;
};

struct Texture_Storage {
    uint16 num_entries;
    uint16 capacity;

    memory_arena* arena;
    Renderer_Texture* textures; // dynarray
};

Texture_Handle Create_New_Texture(Texture_Storage* ts);
void Init_Texture_Storage(Texture_Storage* ts, memory_arena* arena, uint16 max_capacity = 1024) {
    *ts = {};
    ts->num_entries = 0;
    ts->capacity = max_capacity;
    ts->arena = arena;
    ts->textures = CreateArray(arena, Renderer_Texture, max_capacity);

    Texture_Handle nil = Create_New_Texture(ts); // reserve handle 0 as a NULL handle.

    printf("Texture Storage created with %u slots\n", max_capacity);
}

Texture_Handle Create_New_Texture(Texture_Storage* ts) {
    AssertMsg(ts->num_entries < ts->capacity, "Texture_Storage ran out of room to create new texture handles!");

    Texture_Handle new_handle = ts->num_entries;
    ts->num_entries++;
    ArrayAdd(ts->textures);

    Renderer_Texture* new_texture = ArrayPeek(ts->textures);
    memory_set(new_texture, 0, sizeof(*new_texture));

    printf("New Texture Handle created: %u\n", new_handle);

    return new_handle;
}

bool Load_Texture_From_File(Texture_Storage* ts, Texture_Handle handle, wchar_t* filename) {
    printf("Loading '%ws' into texture %u\n", filename, handle);

    ts->textures[handle].gpu_handle = 1;
    ts->textures[handle].width  = 1024;
    ts->textures[handle].height = 1024;
    ts->textures[handle].format = 1;
    ts->textures[handle].name = "texture_name";
    ts->textures[handle].filename = filename;

    return true;
}

Renderer_Texture* Get_Texture_Data(Texture_Storage* ts, Texture_Handle handle) {
    AssertMsg(handle > 0 && handle < ts->capacity, "Invalid Handle");

    return &ts->textures[handle];
};

void print_texture_data(Renderer_Texture* tex) {
    printf("------------------------------------------\n");
    printf(" Texture Info: '%s'\n", tex->name);
    printf(" Filename:     '%ws'\n", tex->filename);
    printf(" Resolution:    %u x %u\n", tex->width, tex->height);
    printf("\n");
}

int main(int argc, char* argv[]) {
    uint64 arena_size = 1024 * 1024;
    uint8* memory = (uint8*)malloc(arena_size);
    memory_arena arena;
    CreateArena(&arena, arena_size, memory);

    Texture_Storage storage;
    Init_Texture_Storage(&storage, &arena);

    Texture_Handle h1 = Create_New_Texture(&storage);
    Renderer_Texture* tex = Get_Texture_Data(&storage, h1);
    print_texture_data(tex);

    Load_Texture_From_File(&storage, h1, L"metal.dds");
    print_texture_data(tex);

    free(memory);

    return 0;
}

#else
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
    InitLogging(true, log_level::LOG_LEVEL_TRACE);
    platform_setup_paths();

    AppConfig config;
    config.application_name = "DX12 Test";
    config.start_x = 20;
    config.start_y = 20;
    config.start_width = 800;
    config.start_height = 800;
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

    //uint32 monitor_refresh_hz = 60;
    uint32 target_framerate = 240;

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
        engine.is_paused = false;

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
                renderer_present(1); // note: runs wayyy faster if its here?

                uint64 WorkCounter = platform_get_wall_clock();
                real32 WorkSecondsElapsed = (real32)platform_get_seconds_elapsed(LastCounter, WorkCounter);

                // TODO: Untested! buggy
                real32 SecondsElapsedForFrame = WorkSecondsElapsed;
                bool32 LockFramerate = false;
                uint32 num_busy_sleep = 0;
                if (LockFramerate) {
                    if (SecondsElapsedForFrame < engine.target_frame_time) {
                        uint64 SleepMS = (uint64)(1000.0f*(engine.target_frame_time - SecondsElapsedForFrame));
                        if (SleepMS > 0) {
                            platform_sleep(SleepMS);
                        }
    
                        while (SecondsElapsedForFrame < engine.target_frame_time) {
                            SecondsElapsedForFrame = (real32)platform_get_seconds_elapsed(LastCounter, platform_get_wall_clock());
                            num_busy_sleep++;
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
                //renderer_present(0);
    
                FlipWallClock = platform_get_wall_clock();

                //uint64 EndCycleCount = __rdtsc();
                //uint64 CyclesElapsed = EndCycleCount - LastCycleCount;
                //LastCycleCount = EndCycleCount;
    
                real32 FPS = 1000.0f / MSPerFrame;
                //real32 MCPF = ((real32)CyclesElapsed / (1000.0f*1000.0f));
    
                #if 0
                RH_TRACE("Frame: %.02f ms  %.02ffps\n", MSPerFrame, FPS);
                #else
                platform_console_set_title("%s: %.02f ms, FPS: %7.02ffps [%3u]", config.application_name, MSPerFrame, FPS, num_busy_sleep);
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
            } else if (context.u16[0] == KEY_P) {
                engine.is_paused = !engine.is_paused;
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
#endif