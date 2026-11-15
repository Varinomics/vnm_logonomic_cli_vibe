#pragma once

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/lifecycle_events.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace vibe {

inline constexpr std::size_t k_max_hook_input_bytes = 1024 * 1024;

logonomic::cli_addon::Lifecycle_declaration lifecycle_declaration();

// Native transcript_path names a session directory. Only session_start records
// carry this opaque resume identity; the helper supplies the resolved VIBE_HOME.
std::optional<logonomic::cli_addon::Provider_event> compact_hook_event(
    std::string_view hook_name,
    const logonomic::cli_addon::Json_value& payload,
    std::string_view vibe_home);

logonomic::cli_addon::Classify_event_result classify_event(
    const logonomic::cli_addon::Classify_event_request& request);

} // namespace vibe
