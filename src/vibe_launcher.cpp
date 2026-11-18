#include "vibe_launcher.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/host_simulator.h>

#include <array>
#include <system_error>

namespace vibe {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

std::optional<Interactive_invocation> interactive_invocation(const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        return std::nullopt;
    }
    const auto name = addon::lower(addon::base_name(argv.front()));
    if (name != "vibe" && name != "vibe.exe") {
        return std::nullopt;
    }
    Interactive_invocation result;
    bool continue_session = false;
    bool resume_session = false;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& arg = argv[index];
        if (arg == "--") {
            if (index + 1 < argv.size()) {
                if (result.initial_prompt || index + 2 != argv.size()) {
                    return std::nullopt;
                }
                result.initial_prompt = true;
            }
            break;
        }
        const auto equal = arg.find('=');
        const auto option = std::string_view(arg).substr(0, equal);
        if (option == "--agent" || option == "--add-dir" || option == "--enabled-tools" || option == "--disabled-tools") {
            result.options.push_back(arg);
            if (equal == std::string::npos) {
                if (++index == argv.size() || argv[index].empty() ||
                    (argv[index].size() > 1 && argv[index].front() == '-'))
                {
                    return std::nullopt;
                }
                result.options.push_back(argv[index]);
            }
            else
            if (equal + 1 == arg.size()) {
                return std::nullopt;
            }
        }
        else
        if (option == "--resume") {
            if (continue_session || (equal != std::string::npos && equal + 1 == arg.size())) {
                return std::nullopt;
            }
            resume_session = true;
            result.saved_session = true;
            if (equal == std::string::npos && index + 1 < argv.size() && !argv[index + 1].starts_with('-')) {
                ++index;
            }
        }
        else
        if (arg == "--continue" || arg == "-c") {
            if (resume_session) {
                return std::nullopt;
            }
            continue_session = true;
            result.saved_session = true;
        }
        else
        if (arg == "--auto-approve" || arg == "--yolo" || arg == "--smart-approve" || arg == "--trust") {
            result.options.push_back(arg);
        }
        else {
            if ((arg.size() > 1 && arg.front() == '-') || arg == "mcp" || arg == "config" ||
                arg == "update" || arg == "run-server" || result.initial_prompt)
            {
                return std::nullopt;
            }
            result.initial_prompt = true;
        }
    }
    return result;
}

bool supported_version(std::string_view version)
{
    if (!addon::is_semantic_version(version)) return false;
    const auto suffix = version.find_first_of("-+");
    auto components = version.substr(0, suffix);
    for (const std::string_view minimum : {"3", "1", "1"}) {
        const auto dot = components.find('.');
        const auto component = components.substr(0, dot);
        if (component.size() != minimum.size()) return component.size() > minimum.size();
        if (component != minimum) return component > minimum;
        components = dot == std::string_view::npos ? std::string_view{} : components.substr(dot + 1);
    }
    // A prerelease of 3.1.1 precedes the release which introduced the hooks.
    return suffix == std::string_view::npos || version[suffix] == '+';
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

std::optional<Native_program> verified_program(std::string_view executable,
    const addon::Environment& environment, std::string_view working_directory, addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        return std::nullopt;
    }
    const auto program = addon::resolve_program(executable, environment, working_directory);
    if (!program) return std::nullopt;
#ifdef _WIN32
    if (!addon::native_image(*program)) return std::nullopt;
#endif
    const auto output = addon::probe({addon::path_to_utf8(*program), "--version"},
        environment, working_directory, 5000, context);
    if (!output) return std::nullopt;
    constexpr std::string_view labels[] = {"vibe", "mistral-vibe", "mistral vibe"};
    const auto stdout_version = addon::parse_version_stream(output->output, labels);
    const auto stderr_version = addon::parse_version_stream(output->errors, labels);
    if (!stdout_version || !stderr_version ||
        (!stdout_version->empty() && !stderr_version->empty() && *stdout_version != *stderr_version)) return std::nullopt;
    const auto& version = stdout_version->empty() ? *stderr_version : *stdout_version;
    if (version.empty()) {
        return std::nullopt;
    }
    return Native_program{*program, version};
}

std::optional<std::string> hook_executable()
{
    const auto root = addon::environment_value(addon::current_process_environment(), addon::k_env_package_root);
    if (root.empty() || !addon::path_from_utf8(root).is_absolute()) return std::nullopt;
#ifdef _WIN32
    constexpr const char* helper = "logonomic_vibe_hook.exe";
#else
    constexpr const char* helper = "logonomic_vibe_hook";
#endif
    const auto path = addon::path_from_utf8(root) / "helpers" / helper;
    if (!addon::regular_file(path)) return std::nullopt;
    return addon::path_to_utf8(path);
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
#ifdef _WIN32
        if (!addon::native_image(*program)) {
            context.succeed(addon::encode_candidates(candidates));
            return;
        }
#endif
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
    patch.disposition = addon::Launch_disposition{false, false, false};
    const auto parsed = invocation ? interactive_invocation(invocation->argv) : std::nullopt;
    const auto environment = addon::environment_pairs(request->launch_environment->effective_environment);
    // Native startup resume can restore another cwd and configuration context.
    // Leave it and explicit cwd overrides unmanaged; exact host resume is
    // verified separately against its saved record and selected directory.
    if (!parsed || parsed->saved_session ||
        !addon::environment_value(environment, "VIBE_CWD").empty()) {
        context.succeed(addon::encode_launch_patch(patch, 3));
        return;
    }
    const auto program = verified_program(invocation->argv.front(), environment, request->working_directory, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (!program || !supported_version(program->version)) {
        context.succeed(addon::encode_launch_patch(patch, 3));
        return;
    }
    const auto home = resolve_home(environment, request->working_directory);
    if (!home) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_home_unavailable");
        return;
    }
    const auto helper = hook_executable();
    if (!helper) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_hook_unavailable");
        return;
    }
    patch.disposition = addon::Launch_disposition{true, true, false};
    patch.apply_managed_configuration = true;
    patch.set = {{"VNM_VIBE_HOOK", *helper}, {"VIBE_HOME", addon::path_to_utf8(*home)}};
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
