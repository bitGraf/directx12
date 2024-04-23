#pragma once

#include "Defines.h"
#include <laml/laml.hpp>

//struct memory_arena;
//struct render_geometry;
//struct render_material;
//struct shader;
//struct ShaderUniform;
//struct frame_buffer;
//struct frame_buffer_attachment;

enum Renderer_Api_Type {
    RENDERER_API_OPENGL,
    RENDERER_API_DIRECTX12
};

enum class Shader_Data_Type : uint8 {
    Invalid = 0, 
    Float, Float2, Float3, Float4,
    Mat3, Mat4, Mat3x4,
    Int, Int2, Int3, Int4,
    Bool,
    UInt
};

enum class Index_Buffer_Type : uint32 {
    UInt16 = 0,
    UInt32
};



// specifies a 'view' into the GPU's copy of a piece of geometry.
// Does not contain the actual data, just a handle to it.
struct Render_Geometry {
    struct _Vertex_Buffer {
        uint64 handle;          // handle to the gpu version of this data
        uint32 buffer_size;     // total buffer size, in bytes
        uint32 buffer_stride;   // stide of buffer element, in bytes
    } vertex_buffer;

    uint32 num_verts;
};

struct Render_Geometry_Indexed {
    struct _Vertex_Buffer {
        uint64 handle;          // handle to the gpu version of this data
        uint32 buffer_size;     // total buffer size, in bytes
        uint32 buffer_stride;   // stide of buffer element, in bytes
    } vertex_buffer;

    struct _Index_Buffer {
        uint64 handle;                // handle to the gpu version of this data
        uint32 buffer_size;           // total buffer size, in bytes
        Index_Buffer_Type index_type;   // index buffer type (16/32 uint)
    } index_buffer;

    uint32 num_verts;
    uint32 num_indices;
};

typedef uint16 Texture_Handle;

struct Renderer_Texture {
    uint64 gpu_handle;
    uint16 width, height;
    uint32 format;

    //char*    name;
    //wchar_t* filename;
    //uint8*   data;
};
