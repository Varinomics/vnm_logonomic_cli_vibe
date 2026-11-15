#include "vibe_launcher.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>

#include <array>
#include <system_error>

namespace vibe {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

bool interactive_invocation(const std::vector<std::string>& argv)
{
    if (argv.empty()) return false;
    const auto name = addon::lower(addon::base_name(argv.front()));
    if (name != "vibe" && name != "vibe.exe" && name != "vibe.cmd" && name != "vibe.bat") return false;
    bool prompt = false;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& arg = argv[index];
        if (arg == "--") return !prompt && index + 2 == argv.size();
        if (arg == "--agent" || arg == "--add-dir") {
            if (++index == argv.size() || argv[index].empty()) return false;
        }
        else if (arg.starts_with("--agent=") || arg.starts_with("--add-dir=")) {
            if (arg.back() == '=') return false;
        }
        else if (arg == "--resume") {
            if (index + 1 < argv.size() && !argv[index + 1].starts_with('-')) ++index;
        }
        else if (arg.starts_with("--resume=")) {
            if (arg.back() == '=') return false;
        }
        else if (arg == "--continue" || arg == "-c" || arg == "--auto-approve" ||
            arg == "--yolo" || arg == "--smart-approve" || arg == "--trust") {
        }
        else if (arg.starts_with('-') || arg == "mcp" || arg == "config" ||
            arg == "update" || arg == "run-server" || prompt) {
            return false;
        }
        else {
            prompt = true;
        }
    }
    return true;
}

std::optional<fs::path> resolve_home(const addon::Environment& environment, std::string_view working_directory)
{
    auto selected = addon::environment_value(environment, "VIBE_HOME");
    if (!selected.empty() && selected != addon::trim(selected)) return std::nullopt;
    if (selected.empty()) {
#ifdef _WIN32
        selected = addon::environment_value(environment, "USERPROFILE");
#else
        selected = addon::environment_value(environment, "HOME");
#endif
        if (selected.empty()) return std::nullopt;
        selected = addon::path_to_utf8(addon::path_from_utf8(selected) / ".vibe");
    }
    // The native selector expands '~' whereas managed-configuration@1 does
    // not. A relative selector can also change meaning after Vibe changes cwd.
    // Refuse either spelling before the host snapshots a different target.
    const auto path = addon::path_from_utf8(selected);
    if (!path.is_absolute() || working_directory.empty()) return std::nullopt;
    std::error_code error;
    const auto canonical = fs::weakly_canonical(path, error);
    if (error || !canonical.is_absolute()) return std::nullopt;
    return canonical;
}

namespace {

void reject(addon::Invocation_context& context, const addon::Contract_failure& error)
{
    context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
}

std::string direct_word(std::string_view value)
{
    std::string result = "\"";
    for (char character : value) result += character == '"' ? "\"\"\"" : std::string(1, character);
    return result + '"';
}

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_discover_request(context.request().payload, &error);
    if (!request) { reject(context, error); return; }
    const auto environment = addon::environment_pairs(request->ambient_environment);
    auto program = addon::resolve_program("vibe", environment, request->default_home_fallback);
    if (!program) {
        const auto home = addon::environment_value(environment,
            request->platform_id.starts_with("windows") ? "USERPROFILE" : "HOME");
        if (!home.empty()) {
            const auto path = addon::path_from_utf8(home) / ".local" / "bin" /
                (request->platform_id.starts_with("windows") ? "vibe.exe" : "vibe");
            program = addon::resolve_program(addon::path_to_utf8(path), environment, request->default_home_fallback);
        }
    }
    std::vector<addon::Launcher_candidate> candidates;
    if (program) {
        addon::Launcher_candidate candidate;
        candidate.candidate_id = "vibe_interactive";
        candidate.display_name = "Mistral Vibe";
        candidate.script_body = direct_word(addon::path_to_utf8(*program));
        candidate.runner_key = addon::preferred_discovery_runner(*program, addon::k_runner_direct);
        candidate.is_default = true;
        candidate.evidence.set("source", addon::Json_value("installed_launcher"));
        candidates.push_back(std::move(candidate));
    }
    context.succeed(addon::encode_candidates(candidates));
}

void recognize(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_recognize_request(context.request().payload, &error);
    if (!request) { reject(context, error); return; }
    addon::Launcher_recognition recognition;
    const auto invocation = addon::literal_launcher_invocation(request->launcher, request->platform_id);
    recognition.recognized = invocation && interactive_invocation(invocation->argv);
    if (recognition.recognized) recognition.display_name = "Mistral Vibe";
    context.succeed(addon::encode_recognition(recognition));
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_prepare_request(context.request().payload, &error, 3);
    if (!request) { reject(context, error); return; }
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    const auto invocation = addon::literal_launcher_invocation(request->launcher,
        request->launch_environment->platform_id, request->launcher_argv);
    patch.disposition = addon::Launch_disposition{
        invocation && interactive_invocation(invocation->argv), false, false};
    context.succeed(addon::encode_launch_patch(patch, 3));
}

} // namespace

void handle_launcher(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == "launcher-discovery" && request.operation == "discover") discover(context);
    else if (request.capability.capability_id == "launcher-discovery" && request.operation == "recognize") recognize(context);
    else if (request.capability.capability_id == "launch-preparation" && request.operation == "prepare") prepare(context);
    else context.reject_unknown_operation();
}

} // namespace vibe
