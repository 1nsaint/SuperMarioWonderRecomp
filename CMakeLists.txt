cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated code of the game (python -m recomp <nso> private/gen/<module> --name <module> --base ...). Never committed.
set(NX_GEN_DIR "${CMAKE_SOURCE_DIR}/private/gen" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand -Wno-unused-function)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

foreach(mod main sdk)
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
    if(ANDROID)
        target_compile_options(game_${mod} PRIVATE -O2 -fPIC)
    endif()
endforeach()

set(NX_RUNTIME_SOURCES
    runtime/src/main.cpp
    runtime/src/loader.cpp
    runtime/src/memory.cpp
    runtime/src/kernel.cpp
    runtime/src/ipc.cpp
    runtime/src/svc_am.cpp
    runtime/src/svc_misc.cpp
    runtime/src/svc_time.cpp
    runtime/src/hle_fs.cpp
    runtime/src/svc_account.cpp
    runtime/src/hle_ro.cpp
    runtime/src/hle_vi.cpp
    runtime/src/hle_hid.cpp
    runtime/src/hle_net.cpp
    runtime/src/hle_audio.cpp
    runtime/src/vk_bridge.cpp
    runtime/src/vk_gen.cpp
    runtime/src/vk_present.cpp
    runtime/src/game_settings.cpp
)

# Vulkan headers (the same registry version tools/gen_vk.py generated from). The loader comes with the GPU driver.
if(DEFINED ENV{VULKAN_SDK})
    set(NX_VULKAN_SDK "$ENV{VULKAN_SDK}" CACHE PATH "Vulkan SDK")
else()
    set(NX_VULKAN_SDK "C:/VulkanSDK/1.4.357.0" CACHE PATH "Vulkan SDK")
endif()

if(ANDROID)
    # NativeActivity app: libhades.so, entry android_main (android_native_app_glue).
    set(GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(hades SHARED ${NX_RUNTIME_SOURCES} runtime/src/platform_android.cpp "${GLUE}/android_native_app_glue.c")
    target_include_directories(hades PRIVATE runtime/include runtime/src "${NX_VULKAN_SDK}/Include" "${GLUE}")
    # Our ANativeActivity_onCreate (platform_android.cpp) runs first, on the UI thread, then calls the glue's.
    set_source_files_properties("${GLUE}/android_native_app_glue.c" PROPERTIES
        COMPILE_DEFINITIONS "ANativeActivity_onCreate=glue_ANativeActivity_onCreate")
    target_link_libraries(hades PRIVATE game_main game_sdk android log aaudio)
    # The glue's ANativeActivity_onCreate must be exported.
    target_link_options(hades PRIVATE -u ANativeActivity_onCreate)
else()
    add_executable(hades ${NX_RUNTIME_SOURCES} runtime/src/platform_win.cpp)
    target_include_directories(hades PRIVATE runtime/include runtime/src "${NX_VULKAN_SDK}/Include")
    target_link_libraries(hades PRIVATE game_main game_sdk xinput xaudio2 ole32)
    if(MSVC)
        # Keep host allocations low so the guest ranges (0x40_0000_0000..0x70_0000_0000) are always free; a big
        # main-thread stack (recompiled calls nest deeply); a link map for tools/symbolize.py.
        target_link_options(hades PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO /MAP)
    endif()
endif()
