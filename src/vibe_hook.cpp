#include "vibe_events.h"
#include "vibe_launcher.h"

#include <logonomic_cli_addon/helper_client.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>

#include <array>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

std::string read_input()
{
    std::string input;
    std::array<char, 4096> buffer{};
    while (std::cin) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (count > vibe::k_max_hook_input_bytes - input.size()) {
            return {};
        }
        input.append(buffer.data(), count);
    }
    return input;
}

void deliver(int argc, char** argv)
{
    addon::Contract_failure failure;
    auto environment = addon::read_helper_environment(&failure);
    if (!environment || environment->package_id != "logonomic.cli.vibe" || argc > 2) {
        return;
    }
    const auto input = read_input();
    addon::Json_parse_limits limits;
    limits.max_bytes = vibe::k_max_hook_input_bytes;
    limits.max_string_bytes = vibe::k_max_hook_input_bytes;
    const auto payload = addon::parse_json(input, limits, nullptr);
    if (!payload || !payload->is_object()) {
        return;
    }
    const auto* event_name = payload->find("hook_event_name");
    if (!event_name || !event_name->is_string() ||
        (argc == 2 && event_name->as_string() != argv[1]))
    {
        return;
    }
    std::error_code error;
    const auto directory = std::filesystem::current_path(error);
    if (error) {
        return;
    }
    const auto directory_utf8 = directory.u8string();
    const std::string directory_text(directory_utf8.begin(), directory_utf8.end());
    const auto home = vibe::resolve_home(addon::current_process_environment(), directory_text);
    if (!home) {
        return;
    }
    const auto home_utf8 = home->u8string();
    const std::string home_text(home_utf8.begin(), home_utf8.end());
    const auto event = vibe::compact_hook_event(event_name->as_string(), *payload, home_text);
    if (!event) {
        return;
    }
    addon::Helper_client client(std::move(*environment));
    const auto key = addon::random_nonce_hex();
    const auto outcome = client.deliver_provider_event(event->name, event->body, key);
    if (outcome.committed) {
        (void)client.acknowledge_provider_event(key);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // This observational hook must not replace prompts or block the CLI. Its
    // stdout is intentionally empty even when provider-event delivery fails.
    try {
        deliver(argc, argv);
    }
    catch (const std::exception&) {
    }
    return 0;
}
