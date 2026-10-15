cmake_minimum_required(VERSION 3.20)
project(FFXII_Core_Regression LANGUAGES CXX)
if(NOT MSVC)
    message(FATAL_ERROR "These tests exercise Windows SEH and require MSVC.")
endif()
enable_testing()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(MOD_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../src" ABSOLUTE)
add_executable(core_regression_tests
    core_regression_tests.cpp
    runtime_stubs.cpp
    "${MOD_SOURCE}/core/game_text.cpp"
    "${MOD_SOURCE}/core/game_text_ex.cpp"
    "${MOD_SOURCE}/core/logger.cpp"
)
target_include_directories(core_regression_tests PRIVATE "${MOD_SOURCE}")
target_compile_options(core_regression_tests PRIVATE /utf-8)
add_test(NAME core_regression COMMAND core_regression_tests)

# Test Tolk lifecycle using a private fake DLL beside each test executable.
# Nothing connects to the user's actual screen reader or game install.
foreach(mode IN ITEMS good missing)
    add_library(fake_tolk_${mode} SHARED fake_tolk.cpp)
    set_target_properties(fake_tolk_${mode} PROPERTIES
        OUTPUT_NAME Tolk
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/speech-${mode}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/speech-${mode}"
    )
    add_executable(speech_${mode}_tests
        speech_regression_tests.cpp runtime_stubs.cpp
        "${MOD_SOURCE}/speech/speech.cpp"
        "${MOD_SOURCE}/speech/phrasebook.cpp"
        "${MOD_SOURCE}/core/logger.cpp"
    )
    set_target_properties(speech_${mode}_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/speech-${mode}"
    )
    target_include_directories(speech_${mode}_tests PRIVATE "${MOD_SOURCE}")
    target_compile_options(speech_${mode}_tests PRIVATE /utf-8)
    add_dependencies(speech_${mode}_tests fake_tolk_${mode})
    if(mode STREQUAL "missing")
        target_compile_definitions(fake_tolk_${mode} PRIVATE OMIT_TOLK_LOAD)
        target_compile_definitions(speech_${mode}_tests PRIVATE OMIT_TOLK_LOAD)
    endif()
    add_test(NAME speech_${mode}_regression COMMAND speech_${mode}_tests)
endforeach()
