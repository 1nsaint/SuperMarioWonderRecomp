cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp CXX)

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
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

foreach(mod main sdk)
    file(GLOB src CONFIGURE_DEPENDS "${NX_GEN_DIR}/${mod}/*.cpp")
    add_library(game_${mod} STATIC ${src})
    target_include_directories(game_${mod} PRIVATE runtime/include "${NX_GEN_DIR}/${mod}")
endforeach()

add_executable(hades
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
)
target_include_directories(hades PRIVATE runtime/include runtime/src)
target_link_libraries(hades PRIVATE game_main game_sdk xinput)
# Static tables (and HLE registrars) must not be dropped by the linker.
if(MSVC)
    # Keep host allocations low so the guest ranges (0x40_0000_0000..0x80_0000_0000) are always free.
    target_link_options(hades PRIVATE /STACK:67108864 /HIGHENTROPYVA:NO)
endif()
