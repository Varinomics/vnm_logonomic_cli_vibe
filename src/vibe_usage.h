#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace vibe {

void handle_usage_scan(logonomic::cli_addon::Invocation_context& context);
void handle_usage_session(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
