cmake_minimum_required(VERSION 3.20)
project(ds3_mod_loader C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "deadspace3.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the proxy has no runtime DLL dependency next to the game.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook (git submodule, pinned to v1.3.4), compiled straight into the proxy.
set(MINHOOK_DIR ${CMAKE_SOURCE_DIR}/third_party/minhook)
add_library(minhook STATIC
    ${MINHOOK_DIR}/src/buffer.c
    ${MINHOOK_DIR}/src/hook.c
    ${MINHOOK_DIR}/src/trampoline.c
    ${MINHOOK_DIR}/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC ${MINHOOK_DIR}/include)

add_library(dinput8 SHARED
    src/proxy.cpp
    src/config.cpp
    src/game.cpp
    src/sealoffers.cpp
    src/input.cpp
    src/log.cpp
    src/modmenu.cpp
    src/mods.cpp
    src/patches.cpp
    src/overlay.cpp
    src/sdk.cpp
    src/dinput8.def
)
target_link_libraries(dinput8 PRIVATE minhook)
target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(dinput8 PRIVATE /W4 /permissive-)
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8)

# The drop-in mods: the samples and the add-ons, the Raw Input Mouse Fix among them. sdk/CMakeLists.txt also builds
# them on its own, from the sdk folder of the source download, with nothing but ds3mod.h. After a build each one is ready to copy
# in build\plugins\<Mod>\.
add_subdirectory(sdk)

