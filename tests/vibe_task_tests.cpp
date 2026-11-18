#include "vibe_tasks.h"
#include "vibe_store_fixture.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <iostream>

namespace {
using namespace vibe_test;

class Invocation final : public addon::Invocation_context
{
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool done = false;
    bool success = false;
    bool cancelled = false;
    addon::Result_status status = addon::Result_status::FAILED;
    std::string reason;
    std::string detail;

    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { output = std::move(result); done = success = true; }
    void fail(addon::Result_status disposition, std::string code, std::string message) override
    { status = disposition; reason = std::move(code); detail = std::move(message); done = true; }
    bool finished() const override { return done; }
};

std::string direct_script(const std::vector<std::string>& argv)
{
    std::string result;
    for (const auto& argument : argv) {
        if (!result.empty()) result += ' ';
        result += '"';
        for (const char character : argument) result += character == '"' ? "\"\"\"" : std::string(1, character);
        result += '"';
    }
    return result;
}

class Task_fixture
{
public:
    Store_fixture directory;
    fs::path program;
    addon::Structured_prepare_request input;

    explicit Task_fixture(const fs::path& fixture)
    {
#ifdef _WIN32
        program = directory.root / "vibe.exe";
#else
        program = directory.root / "vibe";
#endif
        fs::copy_file(fixture, program);
        input.purpose = addon::Structured_purpose::IMPLEMENTATION;
        input.prompt = "\xc2\xa0 \nImplement π.\nPreserve the second line.\r\n\xe3\x80\x80";
        input.context.launcher_argv = {addon::path_to_utf8(program), "--agent", "ask", "--add-dir", "read here", "--trust"};
        input.context.launcher = {direct_script(input.context.launcher_argv), addon::k_runner_direct, {}};
        input.context.working_directory = addon::path_to_utf8(directory.root);
        input.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(directory.root / "profile")};
        auto environment = addon::Json_value::object();
        environment.set("PATH", addon::Json_value(addon::path_to_utf8(directory.root)));
        environment.set("HOME", addon::Json_value(addon::path_to_utf8(directory.root)));
        environment.set("VIBE_HOME", addon::Json_value(addon::path_to_utf8(directory.root / "native")));
        environment.set("MISTRAL_API_KEY", addon::Json_value("fixture-secret-not-returned"));
        environment.set("VIBE_TOOLS", addon::Json_value(R"({"write_file":{"permission":"never"}})"));
        environment.set("VNM_VIBE_HOOK", addon::Json_value("must-not-activate"));
        environment.set("vnm_vibe_other", addon::Json_value("must-not-forward"));
        input.context.launch_environment = addon::Launch_environment{
            addon::host_platform_id(), std::move(environment), std::nullopt};
    }

    Invocation prepare() const
    {
        Invocation call;
        call.input.operation = "prepare";
        call.input.payload = addon::encode_structured_prepare_request(input);
        vibe::handle_tasks(call);
        return call;
    }

    addon::Structured_prepare_result prepared() const
    {
        const auto call = prepare();
        if (!call.success) throw std::runtime_error("task prepare failed: " + call.reason + ": " + call.detail);
        addon::Contract_failure failure;
        const auto result = addon::decode_structured_prepare_result(call.output, &failure);
        if (!result) throw std::runtime_error("invalid task preparation: " + failure.reason_code);
        require(addon::validate_structured_process_proposal(result->process, &failure), "unsafe task proposal");
        return *result;
    }

    void arguments(std::vector<std::string> arguments)
    {
        input.context.launcher_argv = {addon::path_to_utf8(program)};
        input.context.launcher_argv.insert(input.context.launcher_argv.end(), arguments.begin(), arguments.end());
        input.context.launcher.script_body = direct_script(input.context.launcher_argv);
    }
};

addon::Structured_inspect_result inspect(std::string state, std::string bytes, bool final = false,
    int exit_code = 0, bool timed_out = false, std::uint32_t model_version = 0)
{
    addon::Structured_inspect_request request;
    request.purpose = addon::Structured_purpose::IMPLEMENTATION;
    request.inspection = {std::move(state), std::move(bytes), "diagnostics are not an answer", final, timed_out, exit_code};
    request.model_state_version = model_version;
    Invocation call;
    call.input.operation = "inspect";
    call.input.payload = addon::encode_structured_inspect_request(request);
    vibe::handle_tasks(call);
    if (!call.success) throw std::runtime_error("task inspect failed: " + call.reason);
    addon::Contract_failure failure;
    const auto result = addon::decode_structured_inspect_result(call.output, final, &failure);
    if (!result) throw std::runtime_error("invalid task inspection: " + failure.reason_code + ": " + failure.detail);
    require(addon::validate_session_exchange_payload(call.output, &failure), "inspection exceeded wire bounds");
    return *result;
}

addon::Json_value message(std::string_view id, std::string_view role, std::string_view text)
{
    auto entry = json(R"({"id":"entry","sessionId":"owned-session","turnId":"owned-turn","inputEntryId":"input","generationStatus":"completed","type":"message","role":"assistant","content":[],"source":"harness","outcome":{"type":"committed"}})");
    entry.set("id", addon::Json_value(id));
    entry.set("role", addon::Json_value(role));
    auto content = addon::Json_value::array();
    auto block = addon::Json_value::object();
    block.set("type", addon::Json_value("text"));
    block.set("text", addon::Json_value(text));
    content.push_back(std::move(block));
    entry.set("content", std::move(content));
    if (role == "user") {
        entry.set("source", addon::Json_value("turn_start"));
        entry.set("inputEntryId", addon::Json_value());
        entry.set("outcome", addon::Json_value());
    }
    return entry;
}

std::string line(const addon::Json_value& entry)
{
    return addon::serialize_json(entry) + "\n";
}

std::string prefix(const addon::Structured_prepare_result& prepared)
{
    return line(message("input", "user", prepared.process.stdin_payload));
}

void failed(const addon::Structured_inspect_result& result, std::string_view reason)
{
    require(result.complete && result.protocol_state.empty() && !result.result_bytes &&
        result.failure_reason == reason, "native failure was not a complete, isolated failure");
}

void preparation_boundaries(const fs::path& fake_cli)
{
    Task_fixture task(fake_cli);
    const auto result = task.prepared();
    require(result.process.stdin_payload == "Implement π.\nPreserve the second line." &&
        result.process.stdin_behavior == addon::Stdin_behavior::PAYLOAD &&
        !result.inspection.keep_stdin_open && result.inspection.incremental, "native Unicode stdin normalization or EOF changed");
    require(result.process.executable == addon::Process_executable::launcher_program() &&
        result.process.working_directory == addon::Working_directory_intent::SESSION_WORKSPACE &&
        result.process.start == addon::Start_intent::ATTACHED &&
        result.process.argv == std::vector<std::string>({
            "--agent", "ask", "--add-dir", "read here", "--trust", "--output", "streaming", "-p"}),
        "task changed permissions, prompt argv, process ownership or launcher arguments");
    require(result.profile_storage && result.profile_storage->version == task.input.context.profile_storage->version &&
        result.profile_storage->root == task.input.context.profile_storage->root, "task lost the exact profile echo");
    require(result.process.environment == std::vector<std::pair<std::string, std::string>>{{"VNM_VIBE_HOOK", ""}},
        "task copied credential values or left the ambient managed hook enabled");
    const auto& names = result.process.environment_names;
    require(std::find(names.begin(), names.end(), "MISTRAL_API_KEY") != names.end() &&
        std::find(names.begin(), names.end(), "VIBE_TOOLS") != names.end() &&
        std::none_of(names.begin(), names.end(), [](const auto& name) {
            const auto lower = addon::lower(name);
            return lower.starts_with("logonomic_") || lower.starts_with("vnm_vibe_");
        }), "task lost frozen policy inputs or inherited foreground helper authority");
    require(!fs::exists(task.directory.root / "unexpected-launch.txt"), "preparation executed the task");
    task.input.context.launch_environment->effective_environment.set("LOGONOMIC_SESSION_CAPABILITY",
        addon::Json_value("foreign-foreground-authority"));
    require(task.prepare().reason == "frame_violation", "reserved foreground authority was admitted by the SDK");
    task.input.context.launch_environment->effective_environment.erase("LOGONOMIC_SESSION_CAPABILITY");
    task.input.purpose = addon::Structured_purpose::READ_ONLY_REVIEW;
    const auto unsupported = task.prepare();
    require(!unsupported.success && unsupported.status == addon::Result_status::UNAVAILABLE &&
        unsupported.reason == "purpose_unavailable", "undeclared read-only purpose was not refused");
    task.input.purpose = addon::Structured_purpose::IMPLEMENTATION;
    for (const std::vector<std::string>& argv : {
        std::vector<std::string>{"--resume", "saved"}, {"--continue"}, {"another prompt"}, {"--", "prompt"}, {"--worktree"}}) {
        task.arguments(argv);
        require(task.prepare().reason == "vibe_task_launcher_unavailable", "task reused a saved session or foreign prompt");
    }
    task.arguments({});
    task.input.context.launch_environment->effective_environment.set("VIBE_CWD", addon::Json_value("/another-workspace"));
    require(task.prepare().reason == "vibe_task_launcher_unavailable", "task accepted a frozen cwd override");
    task.input.context.launch_environment->effective_environment.erase("VIBE_CWD");
    task.input.context.launcher.script_body = "\"another-vibe\"";
    require(task.prepare().reason == "vibe_task_launcher_unavailable", "task ignored frozen launcher argv mismatch");
    task.arguments({});
    task.input.prompt = "\xc2\xa0\n\t";
    require(task.prepare().reason == "vibe_task_prompt_invalid", "empty native prompt was launched");
    task.input.prompt = "valid";
    write(task.directory.root / "fake-version.txt", "3.1.1\n");
    require(task.prepare().success, "version alone refused the native stream");
}

void incremental_completion(const fs::path& fake_cli)
{
    Task_fixture task(fake_cli);
    const auto prepared = task.prepared();
    const auto answer = message("answer", "assistant", "Completed with π and 雪.\nSecond line.");
    const auto bytes = prefix(prepared) + line(answer);
    auto state = prepared.inspection.protocol_state;
    for (const char byte : bytes) {
        const auto result = inspect(state, std::string(1, byte));
        require(!result.complete && !result.result_bytes && !result.failure_reason,
            "a complete-looking native record authenticated before process exit");
        state = result.protocol_state;
    }
    const auto duplicate = inspect(state, line(answer));
    require(!duplicate.complete && duplicate.presentation.empty(), "duplicate native entry changed the continuation");
    const auto result = inspect(duplicate.protocol_state, "", true, 0, false, 1);
    require(result.complete && result.protocol_state.empty() && result.result_bytes == "Completed with π and 雪.\nSecond line." &&
        !result.failure_reason && !result.model_observation && result.model_failure &&
        result.model_failure->reason_code == "vibe_task_model_unavailable", "native completion or model availability was invented");
    require(result.presentation_state == addon::Presentation_state::AVAILABLE &&
        result.presentation.front().kind == addon::Presentation_event_kind::RESULT_AUTHENTICATED,
        "successful native completion lacked authenticated-result presentation");
    auto conflict = answer;
    conflict.find("content")->as_array().front().set("text", addon::Json_value("different result"));
    failed(inspect(state, line(conflict), true), "vibe_task_conflicting_output");
}

void unsuccessful_native_results(const fs::path& fake_cli)
{
    Task_fixture task(fake_cli);
    const auto prepared = task.prepared();
    const auto state = prepared.inspection.protocol_state;
    const auto bytes = prefix(prepared) + line(message("answer", "assistant", "Plausible but unsuccessful."));
    failed(inspect(state, bytes, true, 1), "startup_failed");
    failed(inspect(state, bytes, true, 2), "vibe_task_process_failed");
    failed(inspect(state, bytes, true, 3), "vibe_task_incomplete");
    failed(inspect(state, bytes, true, 4), "vibe_task_aborted");
    failed(inspect(state, bytes, true, -15), "vibe_task_process_failed");
    failed(inspect(state, bytes, true, 0, true), "vibe_task_timed_out");
    failed(inspect(state, "", true), "vibe_task_missing_input");
    failed(inspect(state, prefix(prepared), true), "vibe_task_missing_answer");
    failed(inspect(state, bytes.substr(0, bytes.size() - 1), true), "vibe_task_truncated_output");
    failed(inspect(state, bytes + "{broken\n", true), "vibe_task_output_invalid");
    failed(inspect("{}", bytes, true), "vibe_task_state_invalid");
    failed(inspect(state, prefix(prepared) + "\xff\n", true), "vibe_task_output_invalid");
    auto answer = message("draft", "assistant", "Discarded draft");
    answer.set("outcome", json(R"({"type":"discarded","cause":"interrupt"})"));
    failed(inspect(state, bytes + line(answer), true), "vibe_task_interrupted");
    failed(inspect(state, prefix(prepared) + line(answer) +
        line(message("contradictory-answer", "assistant", "Cannot undo interruption")), true), "vibe_task_interrupted");
    answer.set("outcome", json(R"({"type":"discarded","cause":"steer"})"));
    failed(inspect(state, bytes + line(answer), true), "vibe_task_foreign_output");
    answer.set("outcome", json(R"({"type":"discarded"})"));
    failed(inspect(state, bytes + line(answer), true), "vibe_task_output_invalid");
    answer.set("outcome", addon::Json_value());
    failed(inspect(state, prefix(prepared) + line(answer), true), "vibe_task_output_invalid");
    answer.set("outcome", json(R"({"type":"discarded","cause":"retry"})"));
    const auto retried = inspect(state, prefix(prepared) + line(answer) +
        line(message("new-answer", "assistant", "Committed retry")), true);
    require(retried.result_bytes == "Committed retry", "successful native retry could not replace discarded draft");
    auto reasoning = json(R"({"id":"dropped-reasoning","type":"reasoning","sessionId":"owned-session","turnId":"owned-turn","generationStatus":"completed","text":"A hook dropped this provisional reasoning","outcome":{"type":"discarded","cause":"failure"}})");
    const auto settled = inspect(state, bytes + line(reasoning), true);
    require(settled.result_bytes == "Plausible but unsuccessful.",
        "discarded reasoning content fate became a false terminal-turn failure");
    reasoning.find("outcome")->set("cause", addon::Json_value("interrupt"));
    failed(inspect(state, bytes + line(reasoning), true), "vibe_task_interrupted");
    auto skipped = message("input", "user", prepared.process.stdin_payload);
    skipped.set("outcome", json(R"({"type":"discarded","cause":"skipped"})"));
    failed(inspect(state, line(skipped) + line(message("answer", "assistant", "Never ran")), true),
        "vibe_task_input_discarded");
}

void identity_and_policy_boundaries(const fs::path& fake_cli)
{
    Task_fixture task(fake_cli);
    const auto prepared = task.prepared();
    const auto state = prepared.inspection.protocol_state;
    auto answer = message("answer", "assistant", "Complete");
    failed(inspect(state, line(answer), true), "vibe_task_prompt_mismatch");
    failed(inspect(state, line(message("input", "user", "unrelated prompt")) + line(answer), true),
        "vibe_task_prompt_mismatch");
    answer.set("sessionId", addon::Json_value("foreign-session"));
    failed(inspect(state, prefix(prepared) + line(answer), true), "vibe_task_foreign_output");
    answer.set("sessionId", addon::Json_value("owned-session"));
    answer.set("turnId", addon::Json_value("foreign-turn"));
    failed(inspect(state, prefix(prepared) + line(answer), true), "vibe_task_foreign_output");
    answer.set("turnId", addon::Json_value("owned-turn"));
    answer.set("inputEntryId", addon::Json_value("foreign-input"));
    failed(inspect(state, prefix(prepared) + line(answer), true), "vibe_task_output_invalid");
    answer.set("inputEntryId", addon::Json_value("input"));
    answer.set("generationStatus", addon::Json_value("in_progress"));
    failed(inspect(state, prefix(prepared) + line(answer), true), "vibe_task_output_invalid");
    answer.set("generationStatus", addon::Json_value("completed"));
    auto tool = json(R"({"id":"tool","type":"effect","sessionId":"owned-session","turnId":"owned-turn","generationStatus":"completed","state":{"status":"failed","error":{"code":"tool_denied","message":"Tool execution denied by approval callback"},"approvalSource":"user"}})");
    failed(inspect(state, prefix(prepared) + line(tool) + line(answer), true), "hook_approval_required");
    tool.find("state")->find("error")->set("code", addon::Json_value("not_found"));
    const auto recovered = inspect(state, prefix(prepared) + line(tool) + line(answer), true);
    require(recovered.result_bytes == "Complete", "recoverable native tool failure replaced authoritative completed turn");
    failed(inspect(state, prefix(prepared) + line(answer) + line(tool), true), "vibe_task_missing_answer");
    const auto pending = inspect(state, std::string(addon::limits::k_max_structured_stream_bytes, 'x'));
    require(!pending.complete, "bounded incomplete line did not retain its state");
    failed(inspect(pending.protocol_state, "x"), "vibe_task_output_limit");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "pass the native fake-CLI fixture executable");
        const auto fake_cli = addon::path_from_utf8(argv[1]);
        preparation_boundaries(fake_cli);
        incremental_completion(fake_cli);
        unsuccessful_native_results(fake_cli);
        identity_and_policy_boundaries(fake_cli);
        std::cout << "Vibe owned task contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
