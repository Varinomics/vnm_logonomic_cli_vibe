#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/conversation_export.h>

namespace vibe {

std::optional<logonomic::cli_addon::Conversation_export_result> export_native_conversation(
    const logonomic::cli_addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled,
    logonomic::cli_addon::Contract_failure* failure);

void handle_export(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
