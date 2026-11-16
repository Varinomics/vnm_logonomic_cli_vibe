#include "vibe_tasks.h"
#include "vibe_launcher.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <logonomic_cli_addon/sha256.h>

#include <array>
#include <map>
#include <string_view>

namespace vibe {
namespace addon = logonomic::cli_addon;

namespace {

constexpr std::string_view k_state_kind = "vibe-owned-task-v1";
constexpr auto k_stream_limit = addon::limits::k_max_structured_stream_bytes;

struct Task_state
{
    std::string prompt_digest;
    std::string session_id;
    std::string turn_id;
    std::string input_entry_id;
    std::string pending;
    std::string answer;
    bool approval_required = false;
    std::map<std::string, std::string> entries;
};

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

const std::string* string_member(const addon::Json_value& object, std::string_view name)
{
    const auto* value = object.find(name);
    return value && value->is_string() ? &value->as_string() : nullptr;
}

bool identity(std::string_view value)
{
    if (value.empty() || value.size() > 1024 || !addon::is_valid_utf8(value)) return false;
    for (const unsigned char character : value) {
        if (character < 0x20 || character == 0x7f) return false;
    }
    return true;
}

std::optional<std::string> normalized_prompt(std::string_view prompt)
{
    if (!addon::is_valid_utf8(prompt) || prompt.find('\0') != std::string_view::npos) return std::nullopt;
    // The SDK's UTF8 whitespace set is Unicode White_Space, the same set
    // Rust str::trim applies to piped headless input.
    return addon::trim(prompt, addon::Launcher_whitespace::UTF8);
}

std::string encode_state(const Task_state& state)
{
    auto value = addon::Json_value::object();
    value.set("kind", addon::Json_value(k_state_kind));
    value.set("prompt_digest", addon::Json_value(state.prompt_digest));
    value.set("session_id", addon::Json_value(state.session_id));
    value.set("turn_id", addon::Json_value(state.turn_id));
    value.set("input_entry_id", addon::Json_value(state.input_entry_id));
    value.set("pending", addon::Json_value(addon::encode_base64(state.pending)));
    value.set("answer", addon::Json_value(state.answer));
    value.set("approval_required", addon::Json_value(state.approval_required));
    auto entries = addon::Json_value::array();
    for (const auto& [id, hash] : state.entries) {
        auto item = addon::Json_value::array();
        item.push_back(addon::Json_value(id));
        item.push_back(addon::Json_value(hash));
        entries.push_back(std::move(item));
    }
    value.set("entries", std::move(entries));
    return addon::serialize_json(value);
}

std::optional<Task_state> decode_state(std::string_view encoded)
{
    const auto value = addon::parse_exchange_protocol_state(encoded);
    if (!value || !value->is_object() || value->as_object().size() != 9) return std::nullopt;
    const auto* kind = string_member(*value, "kind");
    const auto* prompt = string_member(*value, "prompt_digest");
    const auto* session = string_member(*value, "session_id");
    const auto* turn = string_member(*value, "turn_id");
    const auto* input = string_member(*value, "input_entry_id");
    const auto* pending = string_member(*value, "pending");
    const auto* answer = string_member(*value, "answer");
    const auto* approval = value->find("approval_required");
    const auto* entries = value->find("entries");
    if (!kind || *kind != k_state_kind || !prompt || !addon::is_valid_sha256_hex(*prompt) ||
        !session || (!session->empty() && !identity(*session)) ||
        !turn || (!turn->empty() && !identity(*turn)) ||
        !input || (!input->empty() && !identity(*input)) ||
        !pending || !answer || answer->size() > k_stream_limit || answer->find('\0') != std::string::npos ||
        !approval || !approval->is_boolean() || !entries || !entries->is_array()) return std::nullopt;
    const auto bytes = addon::decode_base64(*pending);
    if (!bytes || bytes->size() > k_stream_limit || bytes->find('\n') != std::string::npos ||
        (turn->empty() != input->empty()) || (!turn->empty() && session->empty()) ||
        (turn->empty() && (!answer->empty() || approval->as_boolean()))) return std::nullopt;
    Task_state state;
    state.prompt_digest = *prompt;
    state.session_id = *session;
    state.turn_id = *turn;
    state.input_entry_id = *input;
    state.pending = *bytes;
    state.answer = *answer;
    state.approval_required = approval->as_boolean();
    for (const auto& entry : entries->as_array()) {
        if (!entry.is_array() || entry.as_array().size() != 2 ||
            !entry.as_array()[0].is_string() || !entry.as_array()[1].is_string()) return std::nullopt;
        const auto& id = entry.as_array()[0].as_string();
        const auto& hash = entry.as_array()[1].as_string();
        if (!identity(id) || !addon::is_valid_sha256_hex(hash) || !state.entries.emplace(id, hash).second) return std::nullopt;
    }
    if (!state.input_entry_id.empty() && !state.entries.contains(state.input_entry_id)) return std::nullopt;
    return state;
}

bool supported_task_version(std::string_view version)
{
    if (!addon::is_semantic_version(version)) return false;
    const auto suffix = version.find_first_of("-+");
    auto components = version.substr(0, suffix);
    for (const std::string_view minimum : {"3", "1", "2"}) {
        const auto dot = components.find('.');
        const auto component = components.substr(0, dot);
        if (component.size() != minimum.size()) return component.size() > minimum.size();
        if (component != minimum) return component > minimum;
        components = dot == std::string_view::npos ? std::string_view{} : components.substr(dot + 1);
    }
    return suffix == std::string_view::npos || version[suffix] == '+';
}

bool fresh_invocation(const std::vector<std::string>& argv)
{
    if (!interactive_invocation(argv)) return false;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const auto& argument = argv[index];
        if (argument == "--agent" || argument == "--add-dir") {
            ++index;
        }
        else if (argument.starts_with("--agent=") || argument.starts_with("--add-dir=") ||
            argument == "--auto-approve" || argument == "--yolo" ||
            argument == "--smart-approve" || argument == "--trust") {
        }
        else {
            // A saved-session selector or a second initial prompt would change
            // the task's identity, cwd, configuration or submitted input.
            return false;
        }
    }
    return true;
}

bool task_program(const std::vector<std::string>& argv, const addon::Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context)
{
    const auto program = addon::resolve_program(argv.front(), environment, working_directory);
    if (!program) return false;
#ifdef _WIN32
    if (!addon::native_image(*program)) return false;
#endif
    const auto output = addon::probe({addon::path_to_utf8(*program), "--version"},
        environment, working_directory, 5000, context);
    if (!output) return false;
    constexpr std::string_view labels[] = {"vibe", "mistral-vibe", "mistral vibe"};
    const auto stdout_version = addon::parse_version_stream(output->output, labels);
    const auto stderr_version = addon::parse_version_stream(output->errors, labels);
    if (!stdout_version || !stderr_version ||
        (!stdout_version->empty() && !stderr_version->empty() && *stdout_version != *stderr_version)) return false;
    return supported_task_version(stdout_version->empty() ? *stderr_version : *stdout_version);
}

void reject(addon::Invocation_context& context, const addon::Contract_failure& error)
{
    context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
}

bool purpose_supported(addon::Structured_purpose purpose, addon::Invocation_context& context)
{
    if (purpose == addon::Structured_purpose::IMPLEMENTATION) return true;
    context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable",
        "The native headless CLI does not establish the requested read-only tool policy.");
    return false;
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_structured_prepare_request(context.request().payload, &error);
    if (!request) { reject(context, error); return; }
    if (!purpose_supported(request->purpose, context)) return;
    if (request->context.account_context) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_task_account_unavailable");
        return;
    }
    const auto& source = request->context;
    const auto invocation = addon::literal_launcher_invocation(source.launcher,
        source.launch_environment->platform_id, source.launcher_argv);
    const auto environment = addon::environment_pairs(source.launch_environment->effective_environment);
    if (!invocation || !fresh_invocation(invocation->argv) ||
        !addon::environment_value(environment, "VIBE_CWD").empty()) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_task_launcher_unavailable");
        return;
    }
    const auto prompt = normalized_prompt(request->prompt);
    if (!prompt || prompt->empty()) {
        context.fail(addon::Result_status::FAILED, "vibe_task_prompt_invalid");
        return;
    }
    if (!task_program(invocation->argv, environment, source.working_directory, context)) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "vibe_task_version_unavailable",
            "Mistral Vibe 3.1.2 or newer is required for the qualified headless stream.");
        return;
    }
    addon::Structured_prepare_result result;
    result.profile_storage = source.profile_storage;
    result.process.executable = addon::Process_executable::launcher_program();
    result.process.argv.assign(invocation->argv.begin() + 1, invocation->argv.end());
    result.process.argv.insert(result.process.argv.end(), {"--output", "streaming", "-p"});
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = *prompt;
    for (const auto& [name, value] : environment) {
        (void)value;
        const auto lower = addon::lower(name);
        // These names belong to the interactive helper's routing authority,
        // not to the newly owned headless process.
        if (!lower.starts_with("logonomic_") && !lower.starts_with("vnm_vibe_")) {
            result.process.environment_names.push_back(name);
        }
    }
    // The desktop also composes ambient and authored launcher assignments.
    // An explicit empty override keeps the managed hook inactive even when a
    // launcher was copied from an interactive session's environment.
    result.process.environment.emplace_back("VNM_VIBE_HOOK", "");
    Task_state state;
    state.prompt_digest = digest(*prompt);
    result.inspection.incremental = true;
    result.inspection.protocol_state = encode_state(state);
    if (!addon::validate_structured_process_proposal(result.process, &error)) {
        reject(context, error);
        return;
    }
    auto payload = addon::encode_structured_prepare_result(result);
    if (!addon::validate_session_exchange_payload(payload, &error)) { reject(context, error); return; }
    context.succeed(std::move(payload));
}

std::optional<std::string> message_text(const addon::Json_value& entry, bool text_only)
{
    const auto* content = entry.find("content");
    if (!content || !content->is_array()) return std::nullopt;
    std::string result;
    bool have_text = false;
    for (const auto& block : content->as_array()) {
        const auto* type = string_member(block, "type");
        if (!type) return std::nullopt;
        if (*type == "text") {
            const auto* text = string_member(block, "text");
            if (!text || text->find('\0') != std::string::npos) return std::nullopt;
            if (have_text) result += "\n\n";
            result += *text;
            have_text = true;
        }
        else if (text_only || (*type != "image" && *type != "resource")) {
            return std::nullopt;
        }
        if (result.size() > k_stream_limit) return std::nullopt;
    }
    return result;
}

using Presentation_counts = std::array<std::uint64_t, 4>;

std::optional<std::string> consume_entry(Task_state& state, std::string_view line, Presentation_counts& counts)
{
    const auto entry = addon::parse_json(line, {k_stream_limit}, nullptr);
    if (!entry || !entry->is_object()) return "vibe_task_output_invalid";
    const auto* id = string_member(*entry, "id");
    const auto* session = string_member(*entry, "sessionId");
    const auto* turn = entry->find("turnId");
    const auto* type = string_member(*entry, "type");
    const auto* generation = string_member(*entry, "generationStatus");
    if (!id || !identity(*id) || !session || !identity(*session) ||
        !type || !generation || *generation != "completed" || !turn ||
        (!turn->is_null() && (!turn->is_string() || !identity(turn->as_string())))) return "vibe_task_output_invalid";
    if (!state.session_id.empty() && *session != state.session_id) return "vibe_task_foreign_output";
    if (turn->is_string() && !state.turn_id.empty() && turn->as_string() != state.turn_id) return "vibe_task_foreign_output";
    const auto hash = digest(addon::serialize_json_canonical(*entry));
    const auto found = state.entries.find(*id);
    if (found != state.entries.end()) {
        return found->second == hash ? std::nullopt : std::optional<std::string>("vibe_task_conflicting_output");
    }
    if (*type != "message" && *type != "reasoning" && *type != "effect" &&
        *type != "callback" && *type != "checkpoint" && *type != "notice") return "vibe_task_output_invalid";
    if (state.turn_id.empty() && *type != "message" && *type != "notice" && *type != "checkpoint") {
        return "vibe_task_unbound_output";
    }
    if (*type == "message") {
        const auto* role = string_member(*entry, "role");
        const auto* source = string_member(*entry, "source");
        if (!role || !source || !turn->is_string()) return "vibe_task_output_invalid";
        if (state.turn_id.empty()) {
            const auto text = message_text(*entry, true);
            if (*role != "user" || *source != "turn_start" || !text || digest(*text) != state.prompt_digest) {
                return "vibe_task_prompt_mismatch";
            }
            state.turn_id = turn->as_string();
            state.input_entry_id = *id;
            ++counts[0];
        }
        else if (*source == "turn_start" || *source == "turn_steer") {
            return "vibe_task_foreign_output";
        }
        else if (*role == "assistant" && *source == "harness") {
            const auto text = message_text(*entry, false);
            const auto* input = string_member(*entry, "inputEntryId");
            const auto* outcome = entry->find("outcome");
            const auto* outcome_type = outcome ? string_member(*outcome, "type") : nullptr;
            if (!text || !input || *input != state.input_entry_id || !outcome_type ||
                (*outcome_type != "committed" && *outcome_type != "discarded")) return "vibe_task_output_invalid";
            // A later discarded draft must not authenticate an earlier
            // intermediate response. A successful retry supplies a new answer.
            state.answer = *outcome_type == "committed" ? *text : std::string{};
            ++counts[2];
        }
        else if ((*role != "system" && *role != "user") || *source != "harness") {
            return "vibe_task_output_invalid";
        }
    }
    else if (*type == "effect" || *type == "callback" || *type == "reasoning") {
        if (!turn->is_string() || state.turn_id.empty()) return "vibe_task_unbound_output";
        // Assistant text preceding another tool/reasoning step is not yet the
        // answer to the completed task.
        state.answer.clear();
        if (*type == "reasoning") {
            ++counts[1];
        }
        else if (*type == "effect") {
            ++counts[3];
            const auto* effect = entry->find("state");
            const auto* failure = effect ? effect->find("error") : nullptr;
            const auto* code = failure ? string_member(*failure, "code") : nullptr;
            const auto* message = failure ? string_member(*failure, "message") : nullptr;
            const auto* approval = effect ? string_member(*effect, "approvalSource") : nullptr;
            if (code && *code == "tool_denied" && approval && *approval == "user" && message &&
                *message == "Tool execution denied by approval callback") state.approval_required = true;
        }
    }
    else if (state.turn_id.empty() && !turn->is_null()) {
        return "vibe_task_unbound_output";
    }
    state.session_id = *session;
    state.entries.emplace(*id, hash);
    return std::nullopt;
}

std::optional<std::string> consume_output(Task_state& state, std::string_view bytes, Presentation_counts& counts)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto newline = bytes.find('\n', offset);
        const auto end = newline == std::string_view::npos ? bytes.size() : newline;
        if (end - offset > k_stream_limit - state.pending.size()) return "vibe_task_output_limit";
        state.pending.append(bytes.substr(offset, end - offset));
        if (newline == std::string_view::npos) break;
        if (const auto failure = consume_entry(state, state.pending, counts)) return failure;
        state.pending.clear();
        offset = newline + 1;
    }
    return std::nullopt;
}

addon::Structured_inspect_result completed_failure(std::string reason)
{
    addon::Structured_inspect_result result;
    result.complete = true;
    result.failure_reason = std::move(reason);
    return result;
}

addon::Structured_inspect_result inspect_output(const addon::Structured_inspection& inspection)
{
    auto state = decode_state(inspection.protocol_state);
    if (!state) return completed_failure("vibe_task_state_invalid");
    if (inspection.timed_out) return completed_failure("vibe_task_timed_out");
    Presentation_counts counts{};
    if (const auto failure = consume_output(*state, inspection.stdout_bytes, counts)) return completed_failure(*failure);
    addon::Structured_inspect_result result;
    if (inspection.final) {
        if (inspection.exit_code != 0) {
            if (inspection.exit_code == 1) return completed_failure("startup_failed");
            if (inspection.exit_code == 3) return completed_failure("vibe_task_incomplete");
            if (inspection.exit_code == 4) return completed_failure("vibe_task_aborted");
            return completed_failure("vibe_task_process_failed");
        }
        if (!state->pending.empty()) return completed_failure("vibe_task_truncated_output");
        if (state->approval_required) return completed_failure("hook_approval_required");
        if (state->turn_id.empty()) return completed_failure("vibe_task_missing_input");
        const auto answer = normalized_prompt(state->answer);
        if (!answer || answer->empty()) return completed_failure("vibe_task_missing_answer");
        result.complete = true;
        result.result_bytes = state->answer;
        result.presentation.push_back({addon::Presentation_event_kind::RESULT_AUTHENTICATED, 1});
    }
    else {
        result.protocol_state = encode_state(*state);
        if (!addon::exchange_protocol_state_valid(result.protocol_state)) return completed_failure("vibe_task_output_limit");
    }
    constexpr addon::Presentation_event_kind kinds[] = {
        addon::Presentation_event_kind::REVIEW_INITIALIZED,
        addon::Presentation_event_kind::REASONING,
        addon::Presentation_event_kind::ANALYSIS_UPDATE,
        addon::Presentation_event_kind::TOOL_RESPONSE,
    };
    for (std::size_t index = 0; index < counts.size(); ++index) {
        if (counts[index] > 0) result.presentation.push_back({kinds[index], counts[index]});
    }
    if (!result.presentation.empty()) result.presentation_state = addon::Presentation_state::AVAILABLE;
    return result;
}

void inspect(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_structured_inspect_request(context.request().payload, &error);
    if (!request) { reject(context, error); return; }
    if (!purpose_supported(request->purpose, context)) return;
    auto result = inspect_output(request->inspection);
    result.model_failure = request->model_failure;
    if (request->execution_model_version == 1 && !result.model_failure) {
        // The stream contains no effective-model identity. Do not substitute
        // the selected launcher model or an unrelated interactive session.
        result.model_failure = addon::Contract_failure{"vibe_task_model_unavailable", {}};
    }
    auto payload = addon::encode_structured_inspect_result(result, request->report_failure_diagnostics);
    if (!addon::validate_session_exchange_payload(payload, &error)) {
        auto limited = completed_failure("vibe_task_output_limit");
        limited.model_failure = std::move(result.model_failure);
        payload = addon::encode_structured_inspect_result(limited, request->report_failure_diagnostics);
    }
    context.succeed(std::move(payload));
}

} // namespace

void handle_tasks(addon::Invocation_context& context)
{
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (context.request().operation == addon::k_prepare_operation) prepare(context);
    else if (context.request().operation == addon::k_inspect_operation) inspect(context);
    else context.reject_unknown_operation();
}

} // namespace vibe
