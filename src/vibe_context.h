#pragma once

#include "vibe_store.h"

#include <logonomic_cli_addon/context_state.h>

namespace vibe {

struct Native_context_result
{
    std::optional<logonomic::cli_addon::Context_update> update;
    std::optional<logonomic::cli_addon::Context_failure> failure;
};

// This function performs a fresh owned-store read. A classifier may supply its
// current host revision; observe/poll must omit it. Historical counts are lower
// bounds because native recovery can discard previously completed history.
Native_context_result read_native_context(
    const std::filesystem::path& transcript,
    std::string_view expected_session_id,
    std::optional<std::uint64_t> count_revision = std::nullopt);

} // namespace vibe
