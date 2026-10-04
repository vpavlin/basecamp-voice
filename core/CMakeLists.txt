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
