#pragma once

#include <logonomic_cli_addon/accounts.h>
#include <logonomic_cli_addon/adapter_runtime.h>

#include <filesystem>
#include <optional>

namespace vibe {

bool matches_account(
    const std::optional<std::filesystem::path>& home,
    const logonomic::cli_addon::Account_context& account);
void handle_accounts(logonomic::cli_addon::Invocation_context& context);

} // namespace vibe
