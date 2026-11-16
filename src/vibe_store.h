#pragma once

#include <logonomic_cli_addon/json.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vibe {

enum class Native_session_kind { root, fork, subagent };
enum class Native_child_state {
    reserved, running, idle, interrupted, creation_failed,
    creation_cleanup_pending, turn_failed, deleting_running, deleting_idle, tombstone,
};

struct Native_identity {
    Native_session_kind kind = Native_session_kind::root;
    std::string session_id;
    std::string root_session_id;
    std::optional<std::string> parent_session_id;
};

struct Native_child {
    std::string session_id;
    std::string parent_session_id;
    std::string root_session_id;
    std::string name;
    std::optional<std::string> agent_type;
    std::string spawn_action_id;
    Native_child_state state = Native_child_state::reserved;
    std::optional<std::uint64_t> turn_generation;
    std::string turn_id;

    bool operator==(const Native_child&) const = default;
};

struct Native_snapshot {
    Native_identity identity;
    std::string generation;
    std::uint64_t snapshot_sequence = 0;
    std::string current_sha256;
    // The native session's recorded model pin in this committed generation.
    // This is a selected alias, never evidence of an actual model invocation.
    // A child without its own pin stays unknown; do not fill it from its parent.
    std::optional<std::string> selected_model_alias;
    std::optional<std::string> working_directory;
    std::optional<std::uint64_t> context_window;
    std::vector<Native_child> children;
};

struct Native_store_result {
    std::optional<Native_snapshot> snapshot;
    // Native generations are immutable. An unchanged authenticated CURRENT
    // pointer lets a watcher reuse its last complete child graph cheaply.
    bool unchanged = false;
    std::string reason;
};

// Reads only the committed runtime envelope, never messages or private journal
// inputs. Vibe publishes a new complete generation for each child-graph change.
// All paths are constrained to the hook-attributed unified/<session_id> store.
// Unknown versions, corrupt evidence and racing publications have no snapshot.
Native_store_result read_native_snapshot(
    const std::filesystem::path& transcript,
    std::string_view expected_session_id,
    std::string_view previous_current_sha256 = {});

struct Native_projection_result
{
    std::optional<logonomic::cli_addon::Json_value> projection;
    // The generation is consistent but excludes records awaiting the next
    // native publication. Consumers must not call it the current live state.
    bool journal_pending = false;
    std::string reason;
};

// Loads the public snapshot from the same immutable generation. History chunks
// are independently hashed and restored only when the caller needs them.
Native_projection_result read_native_projection(
    const std::filesystem::path& transcript,
    const Native_snapshot& snapshot,
    bool include_history = true);

bool native_child_active(Native_child_state state);
std::string_view native_child_state_name(Native_child_state state);

} // namespace vibe
