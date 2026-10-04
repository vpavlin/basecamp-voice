cmake_minimum_required(VERSION 3.14)
project(BasecampVoiceCoreTests LANGUAGES CXX)

include(LogosTest)

# The production tools, planner and engine, against a fake Basecamp behind
# the Bus seam (fake_basecamp.h). The module glue (basecamp_voice_core_impl)
# is exercised by the end-to-end run in real Basecamp, not here.
logos_test(
    NAME basecamp_voice_core_tests
    MODULE_SOURCES
        ../src/tools.cpp
        ../src/planner.cpp
        ../src/engine.cpp
        ../src/model_planner.cpp
        ../src/wav.cpp
        ../src/assets.cpp
        ../src/recipes.cpp
        ../src/net.cpp
        ../src/proc.cpp
    TEST_SOURCES
        main.cpp
        test_tools.cpp
        test_planner.cpp
        test_engine.cpp
        test_model_planner.cpp
        test_wav.cpp
        test_assets_context.cpp
        test_review_fixes.cpp
        test_rounds.cpp
        test_recipes.cpp
    EXTRA_INCLUDES
        ../src
    EXTRA_LINK_LIBS
        curl
        crypto
)
