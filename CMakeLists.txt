cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

project(logonomic_cli_vibe VERSION 0.1.0 LANGUAGES CXX)
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

add_library(vibe_provider STATIC src/vibe_launcher.cpp)
target_include_directories(vibe_provider PUBLIC src)
target_link_libraries(vibe_provider PUBLIC logonomic_cli_addon_sdk::sdk)
vibe_target(vibe_provider)
add_executable(vibe_adapter src/adapter.cpp)
target_link_libraries(vibe_adapter PRIVATE vibe_provider)
vibe_target(vibe_adapter)

logonomic_cli_addon_package_platform(VIBE_PLATFORM)
set(VIBE_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(vibe_package logonomic.cli.vibe VIBE_PACKAGE_DIRECTORY
    ADAPTER vibe_adapter)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()
enable_testing()
add_executable(vibe_launcher_tests tests/vibe_launcher_tests.cpp)
target_link_libraries(vibe_launcher_tests PRIVATE vibe_provider)
vibe_target(vibe_launcher_tests)
add_test(NAME vibe_launcher_tests COMMAND vibe_launcher_tests)
add_test(NAME vibe_package_validation COMMAND logonomic_cli_addon_validate
    "${VIBE_PACKAGE_DIRECTORY}" --platform "${VIBE_PLATFORM}")
add_test(NAME vibe_package_conformance COMMAND logonomic_cli_addon_conformance
    "${VIBE_PACKAGE_DIRECTORY}")
set_tests_properties(vibe_launcher_tests vibe_package_validation vibe_package_conformance
    PROPERTIES TIMEOUT 60 LABELS "cli-addon;provider-contract")
