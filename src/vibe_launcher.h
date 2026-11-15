#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vibe {

bool interactive_invocation(const std::vector<std::string>& argv);
std::optional<std::filesystem::path> resolve_home(
    const logonomic::cli_addon::Environment& environment, std::string_view working_directory);
void handle_launcher(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
