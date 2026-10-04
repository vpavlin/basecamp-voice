cmake_minimum_required(VERSION 3.14)
project(BasecampVoiceCoreModulePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME basecamp_voice_core
    SOURCES
        src/basecamp_voice_core_impl.h
        src/basecamp_voice_core_impl.cpp
        src/bus.h
        src/tools.h
        src/tools.cpp
        src/planner.h
        src/planner.cpp
        src/engine.h
        src/engine.cpp
        src/net.h
        src/net.cpp
        src/proc.h
        src/proc.cpp
        src/assets.h
        src/assets.cpp
        src/llm.h
        src/llm.cpp
        src/model_planner.h
        src/model_planner.cpp
        src/system_prompt.inc
        src/plan_schema.inc
        src/recipes.h
        src/recipes.cpp
        recipes/all.inc
        recipes/blockchain.inc
        recipes/storage.inc
        src/voice.h
        src/voice.cpp
        src/wav.cpp
    INCLUDE_DIRS
        src
    # Parakeet + ggml, static (nix/stt), linked into this plugin.
    EXTERNAL_LIBS
        voicestt
    FIND_PACKAGES
        CURL
        OpenSSL
    LINK_LIBRARIES
        CURL::libcurl
        OpenSSL::Crypto
)

# macOS: the microphone is recorded by a separate small app (docs/macos.md).
# Its binary is embedded in the core and written out at first use.
if(APPLE)
    enable_language(OBJC)
    add_executable(voice_recorder macos/voice_recorder.m)
    target_compile_options(voice_recorder PRIVATE -fobjc-arc)
    target_link_libraries(voice_recorder PRIVATE "-framework Foundation" "-framework AVFoundation")
    set(RECORDER_BLOB ${CMAKE_CURRENT_BINARY_DIR}/generated_recorder/recorder_blob.h)
    add_custom_command(OUTPUT ${RECORDER_BLOB}
        COMMAND ${CMAKE_COMMAND} -DIN=$<TARGET_FILE:voice_recorder> -DOUT=${RECORDER_BLOB}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/macos/embed.cmake
        DEPENDS voice_recorder ${CMAKE_CURRENT_SOURCE_DIR}/macos/embed.cmake)
    add_custom_target(recorder_blob DEPENDS ${RECORDER_BLOB})
    add_dependencies(basecamp_voice_core_module_plugin recorder_blob)
    target_include_directories(basecamp_voice_core_module_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated_recorder)
    target_compile_definitions(basecamp_voice_core_module_plugin PRIVATE BASECAMP_VOICE_MAC_RECORDER=1)
endif()
