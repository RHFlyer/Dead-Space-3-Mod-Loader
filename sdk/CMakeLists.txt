# Builds the sample drop-in mods. Works two ways:
#   - on its own, from the sdk folder of the source download (all it needs is ds3mod.h):
#         cmake -S . -B build -A Win32
#         cmake --build build --config Release
#   - as part of the whole proxy repo (the root CMakeLists.txt adds this folder).
# After a build, each mod is ready to copy, as a folder, into the game's plugins folder: build\plugins\<Mod>\.
# sdk\README.md walks through it, and through making your own mod from a sample.

cmake_minimum_required(VERSION 3.20)
project(ds3_mods C CXX)

# Dead Space 3 is a 32-bit game, so mods must be 32-bit DLLs too. "-A Win32" picks that.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "deadspace3.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ds3_mod(<target> <plugins folder name> SOURCES <.cpp files> FILES <files copied next to the DLL>)
# Makes <target>.asi, and copies it with FILES (mod.ini, CHANGELOG.txt) into build\plugins\<folder>\: what you ship.
# Never list the mod's settings ini here: it is the player's, and copying the folder over an older version of the mod
# would reset it (README.md, "Releasing and updating your mod").
function(ds3_mod target folder)
    cmake_parse_arguments(MOD "" "" "SOURCES;FILES" ${ARGN})
    add_library(${target} SHARED ${MOD_SOURCES})
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
    # .asi like most game mods; the static C runtime, so players need no Visual C++ redistributable.
    set_target_properties(${target} PROPERTIES
        SUFFIX ".asi"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(out "${CMAKE_BINARY_DIR}/plugins/${folder}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${out}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${target}>" ${MOD_FILES} "${out}"
        COMMENT "${target}: ready in ${out}")
endfunction()

ds3_mod(HelloMod HelloMod
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/hello_mod/hello_mod.cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/hello_mod/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/hello_mod/CHANGELOG.txt)

ds3_mod(ResourceCounter ResourceCounter
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resource_counter/resource_counter.cpp
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/resource_counter/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/resource_counter/CHANGELOG.txt)

# Mouse Slider Range is a patch mod: no DLL, only its mod.ini, copied so build\plugins\ has every sample.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/mouse_slider_range/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/mouse_slider_range/CHANGELOG.txt
     DESTINATION ${CMAKE_BINARY_DIR}/plugins/MouseSliderRange)

# The Ration Seal Store add-on (API 3). Like the other add-ons below it is built only where its source is, so a copy
# of this folder without it still builds.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/seal_store/seal_store.cpp)
    ds3_mod(SealStore SealStore
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/seal_store/seal_store.cpp
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/seal_store/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/seal_store/CHANGELOG.txt)
endif()

# The Auto Pickup add-on (five hooks, a pickup list drawn with setFont and gradient). Built only where its source is,
# like the Seal Store.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/auto_pickup/auto_pickup.cpp)
    ds3_mod(AutoPickup AutoPickup
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/auto_pickup/auto_pickup.cpp
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/auto_pickup/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/auto_pickup/CHANGELOG.txt)
endif()

# The Raw Input Mouse Fix add-on, based on slopkhayzo's fix (MIT): two hooks, one of them a naked assembly gate, raw
# mouse input read from the game's window, and hot reload. Built only where its source is,
# like the Seal Store. The DLL keeps the standalone fix's name, so an update replaces it in plugins\RawInputMouseFix\.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/raw_mouse/raw_mouse.cpp)
    ds3_mod(DS3RawMouse RawInputMouseFix
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/raw_mouse/raw_mouse.cpp
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/raw_mouse/mod.ini ${CMAKE_CURRENT_SOURCE_DIR}/raw_mouse/CHANGELOG.txt
              ${CMAKE_CURRENT_SOURCE_DIR}/raw_mouse/LICENSE-RawInputMouseFix.txt)
endif()

