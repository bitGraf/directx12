#include "build.h"

int main(int argc, char** argv) {
    for (int n = 1; n < argc; n++) {
        printf("argv[%d] = '%s'\n", n, argv[n]);
    }
    printf("--------------------------------\n");

    project_config conf;
    conf.project_name = "DX12-Test";
    conf.cpp_standard = 14;
    conf.bin_dir = ".\\bin";
    conf.obj_dir = ".\\bin\\int";
    conf.debug_build = true;
    conf.static_link_std = true;
    conf.opt_level = 0;
    conf.opt_intrinsics = true;
    conf.generate_debug_info = true;
    conf.incremental_link = false;
    conf.remove_unref_funcs = true;

    target_config targ;
    targ.target_name = "test";
    targ.type = executable;
    targ.defines = { "RH_INTERNAL", "RH_PLATFORM_WINDOWS" };
    targ.link_dir = "bin";
    targ.link_libs = {};
    targ.include_dirs = relative_dirs("src");
    targ.src_files = find_all_files("src", ".cpp");
    targ.warnings_to_ignore = { /*4189, 4201, 4100, 4723*/ };
    targ.warning_level = 4;
    targ.warnings_are_errors = true;
    targ.subsystem = "console";
    conf.targets.push_back(targ);

    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);

    QueryPerformanceCounter(&start);
    int err_code;
    err_code = build_project(conf);
    //err_code = build_project_incremental(conf);
    QueryPerformanceCounter(&end);

    double elapsed = (double)(end.QuadPart - start.QuadPart) / (double)(freq.QuadPart) * 1000.0;
    if (elapsed > 2000.0)
        printf("%.2f sec elapsed.\n", elapsed/1000.0);
    else
        printf("%.3f ms elapsed.\n", elapsed);

    return err_code;
}