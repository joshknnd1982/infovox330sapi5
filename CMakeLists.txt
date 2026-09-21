cmake_minimum_required(VERSION 3.20)
project(Infovox330SAPI VERSION 1.0.2 LANGUAGES CXX)

# The engine is 32-bit only, so this project is built twice: once as Win32, where the SAPI 5
# DLL loads the engine in-process, and once as x64, where it talks to the 32-bit helper over
# a pipe. Configure with -A Win32 and -A x64 into separate build directories.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(IVX_X64 TRUE)
    message(STATUS "Infovox 330 SAPI5: building x64 (engine out of process)")
else()
    set(IVX_X64 FALSE)
    message(STATUS "Infovox 330 SAPI5: building x86 (engine in process)")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

set(IVX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --------------------------------------------------------------------------------------
# Shared by every binary and both architectures: logging, path discovery, the voice
# catalogue and the parameter mapping. None of it touches the engine.
# --------------------------------------------------------------------------------------
add_library(ivx_common STATIC
    ${IVX_SRC}/ivx_log.cpp
    ${IVX_SRC}/ivx_paths.cpp
    ${IVX_SRC}/ivx_config.cpp
    ${IVX_SRC}/ivx_voices.cpp
    ${IVX_SRC}/ivx_synth.cpp
)
target_include_directories(ivx_common PUBLIC ${IVX_SRC})
target_link_libraries(ivx_common PUBLIC ole32 oleaut32 uuid advapi32 shell32)

set(IVX_TARGETS ivx_common)

# --------------------------------------------------------------------------------------
# The engine driver, the helper process and the sample renderer. 32-bit only, because
# Ivx330nt.dll is.
# --------------------------------------------------------------------------------------
if(NOT IVX_X64)
    add_library(ivx_engine STATIC
        ${IVX_SRC}/ivx_engine.cpp
    )
    target_include_directories(ivx_engine PUBLIC ${IVX_SRC})
    target_link_libraries(ivx_engine PUBLIC ivx_common user32)

    add_executable(ivx_render
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_render.cpp
    )
    target_link_libraries(ivx_render PRIVATE ivx_engine)

    # Measures what the engine actually responds to; see tools/ivx_probe.cpp.
    add_executable(ivx_probe
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_probe.cpp
    )
    target_link_libraries(ivx_probe PRIVATE ivx_engine)

    # The configuration utility. 32-bit so that it can drive the engine in process and
    # speak a preview with settings that have not been saved yet, which is the only way to
    # choose a voice by ear.
    add_executable(Infovox330Config WIN32
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_config_ui.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_config_ui.rc
    )
    target_include_directories(Infovox330Config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_link_libraries(Infovox330Config PRIVATE ivx_engine comctl32 winmm shell32)

    add_executable(Infovox330Server WIN32
        ${IVX_SRC}/ivx_server.cpp
        ${IVX_SRC}/ivx_pipe.cpp
    )
    target_link_libraries(Infovox330Server PRIVATE ivx_engine)

    list(APPEND IVX_TARGETS ivx_engine ivx_render ivx_probe Infovox330Config
         Infovox330Server)
else()
    add_library(ivx_pipe_client STATIC
        ${IVX_SRC}/ivx_pipe.cpp
        ${IVX_SRC}/ivx_pipe_client.cpp
    )
    target_include_directories(ivx_pipe_client PUBLIC ${IVX_SRC})
    target_link_libraries(ivx_pipe_client PUBLIC ivx_common)

    list(APPEND IVX_TARGETS ivx_pipe_client)
endif()

# --------------------------------------------------------------------------------------
# The SAPI 5 engine DLL. Same sources for both architectures; only the backend differs.
# --------------------------------------------------------------------------------------
add_library(Infovox330SAPI5 SHARED
    ${IVX_SRC}/ivx_main.cpp
    ${IVX_SRC}/ivx_com.cpp
    ${IVX_SRC}/ivx_registry.cpp
    ${IVX_SRC}/ivx_datakey.cpp
    ${IVX_SRC}/ivx_token.cpp
    ${IVX_SRC}/ivx_enum_tokens.cpp
    ${IVX_SRC}/ivx_tts_engine.cpp
    ${IVX_SRC}/infovox330_sapi.def
)
target_link_libraries(Infovox330SAPI5 PRIVATE sapi)
if(IVX_X64)
    target_link_libraries(Infovox330SAPI5 PRIVATE ivx_pipe_client)
else()
    target_link_libraries(Infovox330SAPI5 PRIVATE ivx_engine)
endif()
list(APPEND IVX_TARGETS Infovox330SAPI5)

# --------------------------------------------------------------------------------------
# A harness that drives the DLL through its class objects without registering it, so the
# whole engine can be tested without administrator rights.
# --------------------------------------------------------------------------------------
add_executable(ivx_sapitest
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_sapitest.cpp
)
target_link_libraries(ivx_sapitest PRIVATE sapi ole32 oleaut32 uuid)
list(APPEND IVX_TARGETS ivx_sapitest)

# Speaks out loud through the registered SAPI 5 stack. This is the post-install check the
# installer offers to run, and the one that proves registration worked.
add_executable(ivx_speak
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/ivx_speak.cpp
)
target_link_libraries(ivx_speak PRIVATE sapi ole32 oleaut32 uuid)
list(APPEND IVX_TARGETS ivx_speak)

foreach(target ${IVX_TARGETS})
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /MP /utf-8)
        set_property(TARGET ${target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endforeach()
