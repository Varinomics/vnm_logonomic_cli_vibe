cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

project(logonomic_cli_vibe VERSION 0.1.0 LANGUAGES CXX)
# Standalone packages carry executables without a separate MSVC runtime.
# Set this before SDK creation so all linked objects use the same CRT.
if(MSVC AND NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must supply the Logonomic CLI add-on SDK.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

set(VIBE_TOMLPLUSPLUS_SOURCE "" CACHE PATH "Local toml++ source override")
if(VIBE_TOMLPLUSPLUS_SOURCE)
    set(vibe_toml_source "${VIBE_TOMLPLUSPLUS_SOURCE}")
else()
    include(FetchContent)
    FetchContent_Declare(vibe_toml_source
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR logonomic_headers_only)
    FetchContent_MakeAvailable(vibe_toml_source)
    set(vibe_toml_source "${vibe_toml_source_SOURCE_DIR}")
endif()
add_library(vibe_toml INTERFACE)
target_include_directories(vibe_toml SYSTEM INTERFACE "${vibe_toml_source}/include")
# Keep the shipped notice identical to the dependency actually compiled.
file(SHA256 "${vibe_toml_source}/LICENSE" vibe_toml_license)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/tomlplusplus-LICENSE.txt" vibe_shipped_license)
if(NOT vibe_toml_license STREQUAL vibe_shipped_license)
    message(FATAL_ERROR "The selected toml++ license changed; update the packaged notice from that source.")
endif()

function(vibe_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()

add_library(vibe_provider STATIC src/vibe_launcher.cpp src/vibe_config.cpp src/vibe_events.cpp src/vibe_resume.cpp
    src/vibe_store.cpp src/vibe_observer.cpp src/vibe_context.cpp src/vibe_usage.cpp src/vibe_export.cpp
    src/vibe_tasks.cpp)
target_include_directories(vibe_provider PUBLIC src)
target_link_libraries(vibe_provider PUBLIC logonomic_cli_addon_sdk::sdk)
vibe_target(vibe_provider)
target_link_libraries(vibe_provider PRIVATE vibe_toml)
add_executable(vibe_adapter src/adapter.cpp)
target_link_libraries(vibe_adapter PRIVATE vibe_provider)
vibe_target(vibe_adapter)
add_executable(logonomic_vibe_hook src/vibe_hook.cpp)
target_link_libraries(logonomic_vibe_hook PRIVATE vibe_provider)
vibe_target(logonomic_vibe_hook)
logonomic_cli_addon_add_launch_bridge(vibe_launch_bridge)
vibe_target(vibe_launch_bridge)

logonomic_cli_addon_package_platform(VIBE_PLATFORM)
set(VIBE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(vibe_package logonomic.cli.vibe VIBE_PACKAGE_DIRECTORY
    ADAPTER vibe_adapter HELPERS logonomic_vibe_hook vibe_launch_bridge
    INDEX_ARGUMENTS --third-party "adapter/vibe_adapter${CMAKE_EXECUTABLE_SUFFIX}"
        "notices/tomlplusplus-LICENSE.txt" "provenance/tomlplusplus.json")

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
enable_testing()
add_executable(vibe_launcher_tests tests/vibe_launcher_tests.cpp)
target_link_libraries(vibe_launcher_tests PRIVATE vibe_provider)
vibe_target(vibe_launcher_tests)
add_test(NAME vibe_launcher_tests COMMAND vibe_launcher_tests)
add_executable(vibe_config_tests tests/vibe_config_tests.cpp)
target_link_libraries(vibe_config_tests PRIVATE vibe_provider vibe_toml)
vibe_target(vibe_config_tests)
add_test(NAME vibe_config_tests COMMAND vibe_config_tests)
add_executable(vibe_events_tests tests/vibe_events_tests.cpp)
target_link_libraries(vibe_events_tests PRIVATE vibe_provider)
vibe_target(vibe_events_tests)
add_test(NAME vibe_events_tests COMMAND vibe_events_tests
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/lifecycle-events@2.json")
add_executable(vibe_fake_cli tests/vibe_fake_cli.cpp)
vibe_target(vibe_fake_cli)
add_executable(vibe_store_tests tests/vibe_store_tests.cpp)
target_link_libraries(vibe_store_tests PRIVATE vibe_provider)
vibe_target(vibe_store_tests)
add_test(NAME vibe_store_tests COMMAND vibe_store_tests)
add_executable(vibe_projection_tests tests/vibe_projection_tests.cpp)
target_link_libraries(vibe_projection_tests PRIVATE vibe_provider)
vibe_target(vibe_projection_tests)
add_test(NAME vibe_projection_tests COMMAND vibe_projection_tests)
add_executable(vibe_context_tests tests/vibe_context_tests.cpp)
target_link_libraries(vibe_context_tests PRIVATE vibe_provider)
vibe_target(vibe_context_tests)
add_test(NAME vibe_context_tests COMMAND vibe_context_tests)
add_executable(vibe_usage_tests tests/vibe_usage_tests.cpp)
target_link_libraries(vibe_usage_tests PRIVATE vibe_provider)
vibe_target(vibe_usage_tests)
add_test(NAME vibe_usage_tests COMMAND vibe_usage_tests)
add_executable(vibe_export_tests tests/vibe_export_tests.cpp)
target_link_libraries(vibe_export_tests PRIVATE vibe_provider)
vibe_target(vibe_export_tests)
add_test(NAME vibe_export_tests COMMAND vibe_export_tests)
add_executable(vibe_task_tests tests/vibe_task_tests.cpp)
target_link_libraries(vibe_task_tests PRIVATE vibe_provider)
vibe_target(vibe_task_tests)
add_test(NAME vibe_task_tests COMMAND vibe_task_tests "$<TARGET_FILE:vibe_fake_cli>")
add_executable(vibe_observer_tests tests/vibe_observer_tests.cpp)
target_link_libraries(vibe_observer_tests PRIVATE vibe_provider)
vibe_target(vibe_observer_tests)
add_test(NAME vibe_observer_tests COMMAND vibe_observer_tests)
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_test(NAME vibe_hook_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vibe_hook_tests.py"
    "$<TARGET_FILE:logonomic_vibe_hook>" "$<TARGET_FILE:vibe_fake_cli>")
add_test(NAME vibe_protocol_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/vibe_protocol_tests.py"
    "--package=${VIBE_PACKAGE_DIRECTORY}" "--fake-cli=$<TARGET_FILE:vibe_fake_cli>")
add_test(NAME vibe_package_validation COMMAND logonomic_cli_addon_validate
    "${VIBE_PACKAGE_DIRECTORY}" --platform "${VIBE_PLATFORM}")
add_test(NAME vibe_package_conformance COMMAND logonomic_cli_addon_conformance
    "${VIBE_PACKAGE_DIRECTORY}")
set_tests_properties(vibe_launcher_tests vibe_config_tests vibe_events_tests vibe_store_tests vibe_projection_tests vibe_context_tests vibe_usage_tests vibe_export_tests vibe_task_tests vibe_observer_tests vibe_hook_tests
    vibe_protocol_tests vibe_package_validation vibe_package_conformance
    PROPERTIES TIMEOUT 60 LABELS "cli-addon;provider-contract")
