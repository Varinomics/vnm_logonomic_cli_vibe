#include "vibe_resume.h"
#include "vibe_config.h"
#include "vibe_launcher.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace vibe {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

namespace {

const std::string* text(const addon::Json_value& value, std::string_view key)
{
    const auto* member = value.find(key);
    return member && member->is_string() ? &member->as_string() : nullptr;
}

bool session_id_valid(std::string_view value)
{
    // Preserve the entire hook-provided ID. Never abbreviate it into Vibe's
    // prefix selector, accept an option, or turn it into a filesystem path.
    return !value.empty() && value.size() <= 256 && value.front() != '-' && value.front() != '.' &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') || character == '-' || character == '_';
        });
}

std::optional<std::string> read_bounded(const fs::path& path, std::size_t maximum)
{
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(maximum + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || bytes.size() > maximum) return std::nullopt;
    return bytes;
}

bool matches_home(const Resume_record& record, const addon::Session_launch_context& context)
{
    if (!context.launch_environment) return false;
    const auto environment = addon::environment_pairs(context.launch_environment->effective_environment);
    if (!addon::environment_value(environment, "VIBE_CWD").empty()) return false;
    const auto home = resolve_home(environment, context.working_directory);
    if (!home) return false;
    std::error_code error;
    // Existing aliases (including Windows path casing) identify the same home.
    if (fs::equivalent(*home, record.home, error) && !error) return true;
    error.clear();
    const auto recorded_home = fs::weakly_canonical(record.home, error);
    return !error && *home == recorded_home;
}

bool installed_hooks(const fs::path& home)
{
    const auto content = read_bounded(home / "hooks.toml", 768u * 1024u);
    if (!content) return false;
    try {
        return updated_hooks(*content) == *content;
    }
    catch (const std::exception&) {
        return false;
    }
}

void inspect(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_resume_inspect_request(context.request().payload, &error);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    addon::Resume_inspect_result result;
    result.request_digest_hex = addon::resume_inspect_request_digest_hex(*request);
    const auto record = decode_resume_record(request->resume_record);
    if (record && matches_home(*record, request->context)) result.availability = inspect_native_session(*record);
    context.succeed(addon::encode_resume_inspect_result(result, 3));
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &error, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    const auto record = decode_resume_record(request->resume_record);
    if (!record) {
        context.fail(addon::Result_status::FAILED, "resume_record_invalid");
        return;
    }
    if (!matches_home(*record, request->context)) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_resume_home_mismatch");
        return;
    }
    // Vibe keeps a fresh session attached when a native --resume fails. Refuse
    // missing/unknown local evidence here instead of predictably launching it.
    if (inspect_native_session(*record) != addon::Resume_availability::AVAILABLE) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_native_session_unavailable");
        return;
    }
    const auto invocation = addon::literal_launcher_invocation(request->context.launcher,
        request->context.launch_environment->platform_id, request->context.launcher_argv);
    const auto arguments = invocation ? resume_arguments(invocation->argv) : std::nullopt;
    if (!arguments) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_resume_launcher_unavailable");
        return;
    }
    const auto environment = addon::environment_pairs(request->context.launch_environment->effective_environment);
    const auto program = verified_program(invocation->argv, environment, request->context.working_directory, context);
    if (!program) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "vibe_hooks_version_unavailable");
        return;
    }
    const auto helper = hook_executable();
    if (!helper || !installed_hooks(record->home)) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_resume_hooks_unavailable");
        return;
    }
    addon::Resume_prepare_result result;
#ifdef _WIN32
    result.process.executable = "helpers/vibe_launch_bridge.exe";
#else
    result.process.executable = "helpers/vibe_launch_bridge";
#endif
    result.process.argv = {addon::path_to_utf8(*program)};
    result.process.argv.insert(result.process.argv.end(), arguments->begin(), arguments->end());
    result.process.argv.insert(result.process.argv.end(), {"--resume", record->session_id});
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.process.cleanup = addon::Cleanup_intent::RETAIN_SCRATCH;
    result.patch.profile_storage = request->context.profile_storage;
    result.patch.account_context = request->context.account_context;
    result.patch.set = {{"VNM_VIBE_HOOK", *helper}, {"VIBE_HOME", addon::path_to_utf8(record->home)}};
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(request->resume_record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

} // namespace

std::optional<Resume_record> decode_resume_record(const addon::Json_value& record)
{
    const auto* version = record.find("version");
    const auto* session_id = text(record, "provider_session_id");
    const auto* home = text(record, "vibe_home");
    const auto* transcript = text(record, "transcript_path");
    if (!record.is_object() || record.as_object().size() != 4 || !version ||
        !version->is_integer() || version->as_integer() != 1 || !session_id || !session_id_valid(*session_id) ||
        !home || !transcript || home->size() > 4096 || transcript->size() > 4096) return std::nullopt;
    const auto home_path = addon::normalized_resume_directory(*home);
    const auto transcript_path = addon::normalized_resume_directory(*transcript);
    if (!home_path || !transcript_path) return std::nullopt;
    return Resume_record{*session_id, *home_path, *transcript_path};
}

std::optional<std::vector<std::string>> resume_arguments(const std::vector<std::string>& invocation)
{
    if (!interactive_invocation(invocation)) return std::nullopt;
    std::vector<std::string> arguments;
    for (std::size_t index = 1; index < invocation.size(); ++index) {
        const auto& arg = invocation[index];
        if (arg == "--") break; // The original launch's prompt is not sent twice.
        if (arg == "--continue" || arg == "-c" || arg.starts_with("--resume=")) continue;
        if (arg == "--resume") {
            if (index + 1 < invocation.size() && !invocation[index + 1].starts_with('-')) ++index;
            continue;
        }
        if (arg == "--agent" || arg == "--add-dir") {
            arguments.push_back(arg);
            arguments.push_back(invocation[++index]);
        }
        else if (arg.starts_with('-')) arguments.push_back(arg);
    }
    return arguments;
}

addon::Resume_availability inspect_native_session(const Resume_record& record)
{
    // meta.json is the Vibe-owned filesystem metadata sidecar. CURRENT proves
    // that this exact session also has a persisted unified runtime generation.
    // We do not replay Vibe's private journal or infer transcript emptiness.
    const auto metadata_bytes = read_bounded(record.transcript / "meta.json", 1024u * 1024u);
    const auto current_bytes = read_bounded(record.transcript / "CURRENT", 64u * 1024u);
    if (!metadata_bytes || !current_bytes) return addon::Resume_availability::UNKNOWN;
    const auto metadata = addon::parse_json(*metadata_bytes, {}, nullptr);
    const auto current = addon::parse_json(*current_bytes, {}, nullptr);
    if (!metadata || !current) return addon::Resume_availability::UNKNOWN;
    const auto* metadata_id = text(*metadata, "session_id");
    const auto* current_id = text(*current, "session_id");
    const auto* format = text(*current, "store_format");
    const auto* generation = text(*current, "generation");
    const auto* expected_digest = text(*current, "manifest_sha256");
    const auto* sequence = current->find("snapshot_sequence");
    if (!metadata_id || *metadata_id != record.session_id || !current_id || *current_id != record.session_id ||
        !format || *format != "mistral.vibe.unified-session-store/v1" || !generation || generation->size() != 16 ||
        !std::all_of(generation->begin(), generation->end(), [](char digit) { return digit >= '0' && digit <= '9'; }) ||
        !expected_digest || !sequence || !sequence->is_integer() || sequence->as_integer() < 0)
    {
        return addon::Resume_availability::UNKNOWN;
    }
    const auto manifest_bytes = read_bounded(record.transcript / "generations" / *generation / "manifest.json", 64u * 1024u);
    if (!manifest_bytes || manifest_bytes->empty() || manifest_bytes->back() != '\n') {
        return addon::Resume_availability::UNKNOWN;
    }
    // Vibe hashes its canonical JSON document without its terminating LF.
    // Hash the native bytes; the SDK's canonical number spelling can differ.
    const auto body = std::string_view(*manifest_bytes).substr(0, manifest_bytes->size() - 1);
    if (addon::to_hex(addon::Sha256::digest(body)) != *expected_digest) return addon::Resume_availability::UNKNOWN;
    const auto manifest = addon::parse_json(body, {}, nullptr);
    if (!manifest) return addon::Resume_availability::UNKNOWN;
    const auto* manifest_id = text(*manifest, "session_id");
    const auto* manifest_generation = text(*manifest, "generation");
    const auto* manifest_version = manifest->find("manifest_version");
    const auto* manifest_sequence = manifest->find("snapshot_sequence");
    if (!manifest_id || *manifest_id != record.session_id || !manifest_generation || *manifest_generation != *generation ||
        !manifest_version || !manifest_version->is_integer() || manifest_version->as_integer() != 1 ||
        !manifest_sequence || *manifest_sequence != *sequence)
    {
        return addon::Resume_availability::UNKNOWN;
    }
    // A changing pointer is not a stable inspection. Unknown never hides or
    // deletes remembered history, including moved or newer native formats.
    return read_bounded(record.transcript / "CURRENT", 64u * 1024u) == current_bytes ?
        addon::Resume_availability::AVAILABLE : addon::Resume_availability::UNKNOWN;
}

void handle_resume(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.version == 2 && request.operation == "prepare_resume") prepare(context);
    else if (request.capability.version == 3 && request.operation == "inspect_resume") inspect(context);
    else context.reject_unknown_operation();
}

} // namespace vibe
