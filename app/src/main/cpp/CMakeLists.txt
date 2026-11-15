cmake_minimum_required(VERSION 3.22.1)
project(sekiro_native LANGUAGES C CXX)

add_library(sekiro_native SHARED
    assets/bnd4.cpp
    assets/flver.cpp
    assets/tpf.cpp
    main.cpp
    mesh_preview.cpp
    preview_texture.cpp
    vulkan_renderer.cpp
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
)
target_include_directories(sekiro_native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${ANDROID_NDK}/sources/android/native_app_glue"
)
target_compile_features(sekiro_native PRIVATE cxx_std_20)
target_compile_definitions(sekiro_native PRIVATE VK_USE_PLATFORM_ANDROID_KHR $<$<CONFIG:Debug>:SEKIRO_DEBUG>)
target_compile_options(sekiro_native PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-missing-field-initializers>
)
target_link_libraries(sekiro_native PRIVATE android log vulkan)
