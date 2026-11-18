#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vibe {

struct Interactive_invocation
{
    std::vector<std::string> options;
    bool saved_session = false;
    bool initial_prompt = false;
};

struct Native_program
{
    std::filesystem::path path;
    std::string version;
};

std::optional<Interactive_invocation> interactive_invocation(const std::vector<std::string>& argv);
bool supported_version(std::string_view version);
std::optional<std::filesystem::path> resolve_home(
    const logonomic::cli_addon::Environment& environment, std::string_view working_directory);
std::optional<Native_program> verified_program(
    std::string_view executable,
    const logonomic::cli_addon::Environment& environment, std::string_view working_directory,
    logonomic::cli_addon::Invocation_context& context);
std::optional<std::string> hook_executable();
void handle_launcher(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
