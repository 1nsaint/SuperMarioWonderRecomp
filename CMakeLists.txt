cmake_minimum_required(VERSION 3.25)
project(SwitchGamesRecomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated code of the game (python -m recomp <main nso> private/gen/main). Never committed.
set(NX_GEN_DIR "${CMAKE_SOURCE_DIR}/private/gen/main" CACHE PATH "Recompiled module sources")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-label -Wno-unused-value -Wno-parentheses-equality -Wno-unused-variable
                        -Wno-unused-but-set-variable -Wno-constant-logical-operand)
endif()
if(MSVC)
    add_compile_options(/arch:AVX2)
endif()

file(GLOB NX_GEN_SOURCES CONFIGURE_DEPENDS "${NX_GEN_DIR}/*.cpp")
add_library(game_code STATIC ${NX_GEN_SOURCES})
target_include_directories(game_code PUBLIC runtime/include "${NX_GEN_DIR}")
