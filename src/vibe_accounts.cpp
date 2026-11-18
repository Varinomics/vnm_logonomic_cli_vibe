#include "vibe_accounts.h"
#include "vibe_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <string>
#include <system_error>

namespace vibe {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

namespace {

std::optional<std::string> account_key(const fs::path& home)
{
#ifdef _WIN32
    const auto& native = home.native();
    const int size = static_cast<int>(native.size());
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        native.data(), size, nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0) {
        return std::nullopt;
    }
    std::wstring folded(count, L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        native.data(), size, folded.data(), count, nullptr, nullptr, 0) != count)
    {
        return std::nullopt;
    }
    const auto identity = addon::path_to_utf8(fs::path(folded));
#else
    const auto identity = addon::path_to_utf8(home);
#endif
    return "vibe-profile:" + addon::to_hex(addon::Sha256::digest(identity));
}

} // namespace

bool matches_account(const std::optional<fs::path>& home, const addon::Account_context& account)
{
    if (!home || account_key(*home) != account.adapter_account_key) {
        return false;
    }
    if (account.version == 1) {
        return true;
    }
    if (!account.profile_path) {
        return false;
    }
    const auto retained = addon::path_from_utf8(*account.profile_path);
    return retained.is_absolute() && account_key(retained) == account.adapter_account_key;
}

void handle_accounts(addon::Invocation_context& context)
{
    const auto& operation = context.request().operation;
    if (operation != addon::k_accounts_discover_operation && operation != addon::k_accounts_resolve_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure error;
    const auto request = addon::decode_accounts_launcher_request(context.request().payload, &error);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code);
        return;
    }
    const bool discover = operation == addon::k_accounts_discover_operation;
    const auto invocation = addon::literal_launcher_invocation(request->launcher, request->launch_environment.platform_id);
    if (!invocation || !interactive_invocation(invocation->argv)) {
        context.succeed(discover ? addon::encode_accounts({}) : addon::encode_account_resolution({std::nullopt}));
        return;
    }
    auto environment = addon::environment_pairs(request->launch_environment.effective_environment);
#ifdef _WIN32
    constexpr auto home_selector = "USERPROFILE";
#else
    constexpr auto home_selector = "HOME";
#endif
    if (addon::environment_value(environment, home_selector).empty() && !request->default_home_fallback.empty()) {
        addon::set_environment(environment, home_selector, request->default_home_fallback);
    }
    const auto home = resolve_home(environment, request->normalization_directory);
    const auto key = home ? account_key(*home) : std::nullopt;
    if (!key) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_home_unavailable");
        return;
    }
    if (!discover) {
        context.succeed(addon::encode_account_resolution({*key}));
        return;
    }

    // Vibe defines one home per invocation and has no catalogue of sibling homes.
    std::error_code directory_error;
    const auto directory = fs::status(*home, directory_error);
    const auto status = directory.type() == fs::file_type::not_found ? addon::Account_status::UNKNOWN :
        !directory_error && fs::is_directory(directory) ? addon::Account_status::AVAILABLE : addon::Account_status::UNAVAILABLE;
    const auto path = addon::path_to_utf8(*home);
    const auto inventory = addon::encode_accounts({{*key, addon::account_display_name(path), status, path}});
    if (!addon::decode_accounts_result(inventory, &error)) {
        context.fail(addon::Result_status::UNAVAILABLE, error.reason_code);
        return;
    }
    context.succeed(inventory);
}

} // namespace vibe
