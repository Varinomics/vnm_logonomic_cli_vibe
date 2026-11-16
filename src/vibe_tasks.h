#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace vibe {

// Prepare and inspect a fresh, host-owned native headless process. This route
// does not observe or control an existing interactive Vibe session.
void handle_tasks(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
