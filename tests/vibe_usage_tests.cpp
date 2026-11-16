#include "vibe_usage.h"
#include "vibe_projection_fixture.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_session.h>

#include <iostream>
#include <set>

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
    std::string reason;

    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override { return cancelled; }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value result) override { output = std::move(result); done = success = true; }
    void fail(addon::Result_status, std::string code, std::string) override { reason = std::move(code); done = true; }
    bool finished() const override { return done; }
};

addon::Usage_scan_request request(const Store_fixture& store,
    const std::optional<addon::Usage_cache_reference>& accepted = std::nullopt)
{
    static unsigned sequence = 0;
    const auto profile = store.root / "provider-cache";
    const auto candidate = profile / ("candidate-" + std::to_string(++sequence));
    fs::create_directories(candidate);
    addon::Usage_scan_request result;
    result.context.launcher = {"vibe", addon::k_runner_direct, {}};
    result.context.launcher_argv = {"vibe"};
    result.context.working_directory = addon::path_to_utf8(store.root);
    result.context.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(profile)};
    result.context.launch_environment = addon::Launch_environment{
        addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    auto& environment = result.context.launch_environment->effective_environment;
    environment.set("VIBE_HOME", addon::Json_value(addon::path_to_utf8(store.root / "vibe-home")));
    environment.set("VIBE_SESSION_LOGGING__SAVE_DIR", addon::Json_value(addon::path_to_utf8(store.root)));
    result.cache_selection = addon::Usage_cache_selection{accepted, addon::path_to_utf8(candidate)};
    return result;
}

Invocation scan(const addon::Usage_scan_request& request)
{
    Invocation call;
    call.input.operation = "scan";
    call.input.payload = addon::encode_usage_scan_request(request);
    vibe::handle_usage_scan(call);
    return call;
}

addon::Usage_scan_page page(const Invocation& call)
{
    if (!call.success) {
        throw std::runtime_error("scan failed: " + call.reason);
    }
    addon::Contract_failure failure;
    const auto result = addon::decode_usage_scan_result(call.output, &failure);
    if (!result) {
        throw std::runtime_error("invalid usage page: " + failure.reason_code + ": " + failure.detail);
    }
    return *result;
}

void counters(addon::Json_value& value, std::uint64_t input, std::uint64_t output, std::uint64_t cached = 0)
{
    auto usage = addon::Json_value::object();
    usage.set("inputTokens", addon::Json_value(input));
    usage.set("outputTokens", addon::Json_value(output));
    usage.set("totalTokens", addon::Json_value(input + output));
    usage.set("cachedInputTokens", addon::Json_value(cached));
    value.find("snapshot")->find("session")->set("tokenUsage", std::move(usage));
}

void cumulative_accounting()
{
    Store_fixture store;
    auto state = runtime();
    state.find("session_metadata")->set("cwd", addon::Json_value(addon::path_to_utf8(store.root)));
    store.publish(state);
    auto value = projection();
    publish_projection(store, value);
    const auto earlier_current = store.current;
    const auto first = page(scan(request(store)));
    require(first.complete && first.incomplete_historical_coverage && first.records.size() == 1,
        "native usage source omitted its cumulative session or claimed exhaustive history");
    const auto& record = first.records.front();
    require(record.total_tokens == 150 && record.input.value == 120 && record.output.value == 30 &&
        record.semantics == addon::Usage_counter_semantics::CUMULATIVE_FINAL &&
        record.session_key == "root-session" && record.aggregate_evidence == "partial",
        "native cumulative usage was converted into repeated additive deltas");
    require(!record.model && !record.repository_path && !record.cache_read.value &&
        !record.cache_creation.value && record.cache_read.coverage->support == addon::Usage_support::UNKNOWN &&
        record.temporal && record.temporal->unavailable_tokens == 150 && record.temporal->portions.empty(),
        "selected model, cwd, missing cache counts, or snapshot date became unsupported attribution");
    const auto unchanged = page(scan(request(store, first.cache_candidate)));
    require(unchanged.records.front().record_id == record.record_id && unchanged.records.front().total_tokens == 150,
        "repeated cumulative scan double-counted existing usage");

    state.find("session_metadata")->set("active_model", addon::Json_value("model-after-switch"));
    store.publish(state);
    counters(value, 200, 50, 80);
    publish_projection(store, value);
    const auto changed = page(scan(request(store, unchanged.cache_candidate)));
    const auto& next = changed.records.front();
    require(next.total_tokens == 250 && !next.model && next.cache_read.value == 80 &&
        next.cache_read.coverage->covered_records == 0 &&
        next.cache_read.coverage->support == addon::Usage_support::UNKNOWN,
        "model switch reattributed old billing or positive cache lower bound claimed complete coverage");
    write(store.transcript / "CURRENT", canonical(earlier_current) + "\n");
    const auto regressed = scan(request(store, changed.cache_candidate));
    require(!regressed.success && regressed.reason == "vibe_usage_source_regressed",
        "restored older native generation replaced accepted source evidence");
    write(store.transcript / "CURRENT", canonical(store.current) + "\n");
    Invocation describe;
    describe.input.operation = "describe";
    describe.input.payload = addon::encode_usage_session_request({
        {next.record_id, next.session_key}, {"unknown-record", "root-session"}});
    vibe::handle_usage_session(describe);
    addon::Contract_failure failure;
    const auto descriptions = addon::decode_usage_session_result(describe.output, &failure);
    require(describe.success && descriptions && descriptions->front().session &&
        descriptions->front().session->provider_session_id == "root-session" &&
        !descriptions->front().session->excludes_turn_in_flight && !descriptions->back().session,
        "usage session mapping invented identity or completed-turn cutoff");
}

void source_loss_and_resets()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    publish_projection(store, value);
    const auto first = page(scan(request(store)));
    fs::rename(store.transcript, store.root / "removed-session");
    const auto retained = page(scan(request(store, first.cache_candidate)));
    require(retained.records.size() == 1 && retained.records.front().total_tokens == 150,
        "disappearing native history discarded selected cached evidence");
    fs::rename(store.root / "removed-session", store.transcript);
    store.publish(runtime());
    counters(value, 2, 1);
    publish_projection(store, value);
    const auto rejected_request = request(store, retained.cache_candidate);
    const auto rejected = scan(rejected_request);
    require(!rejected.success && rejected.reason == "vibe_usage_counter_regressed" &&
        !fs::exists(addon::path_from_utf8(rejected_request.cache_selection->candidate_root) / "cache-manifest.json"),
        "native counter reset silently replaced accepted cumulative evidence");
    auto reset = request(store);
    reset.mode = "reset";
    require(page(scan(reset)).records.front().total_tokens == 3,
        "explicit reset failed to reconstruct current native sources");

    write(addon::path_from_utf8(retained.cache_candidate->root) / "records.jsonl", "corrupt cache\n");
    const auto corrupt = scan(request(store, retained.cache_candidate));
    require(!corrupt.success && corrupt.reason == "usage_cache_content_mismatch",
        "corrupt selected cache fell back to empty/current native usage");
}

void distinct_native_children()
{
    Store_fixture root;
    root.publish(runtime());
    publish_projection(root, projection());
    Store_fixture child_store("child-session");
    auto state = runtime("child-session");
    state.find("identity")->set("kind", addon::Json_value("subagent"));
    state.find("identity")->set("depth", addon::Json_value(1));
    state.find("identity")->set("parent_session_id", addon::Json_value("root-session"));
    state.find("identity")->set("root_session_id", addon::Json_value("root-session"));
    state.find("session_metadata")->set("parent_session_id", addon::Json_value("root-session"));
    state.find("session_metadata")->set("root_session_id", addon::Json_value("root-session"));
    state.find("session_metadata")->set("subagent_spawn_key", addon::Json_value("spawn-key"));
    state.find("session_metadata")->set("active_model", addon::Json_value());
    state.set("subagents", addon::Json_value());
    child_store.publish(state);
    auto child_projection = projection("child-session");
    counters(child_projection, 20, 5);
    child_projection.find("snapshot")->find("session")->find("tokenUsage")->erase("cachedInputTokens");
    publish_projection(child_store, child_projection);
    fs::copy(child_store.transcript, root.root / "unified" / "child-session", fs::copy_options::recursive);
    const auto result = page(scan(request(root)));
    require(result.records.size() == 2 &&
        result.records.front().total_tokens + result.records.back().total_tokens == 175 &&
        !result.records.front().model && !result.records.back().model,
        "native child usage was lost, double-counted, or attributed to parent's selected model");
}

void forked_history_is_not_billing()
{
    Store_fixture root;
    root.publish(runtime());
    publish_projection(root, projection());
    Store_fixture fork("fork-session");
    auto state = runtime("fork-session");
    state.find("identity")->set("kind", addon::Json_value("fork"));
    state.find("identity")->set("parent_session_id", addon::Json_value("root-session"));
    state.find("identity")->set("root_session_id", addon::Json_value("root-session"));
    state.find("session_metadata")->set("parent_session_id", addon::Json_value("root-session"));
    state.find("session_metadata")->set("root_session_id", addon::Json_value("root-session"));
    fork.publish(state);
    auto inherited = projection("fork-session");
    inherited.find("snapshot")->find("session")->set("tokenUsage", addon::Json_value());
    inherited.find("snapshot")->find("session")->set("contextUsage", addon::Json_value());
    inherited.find("snapshot")->find("history")->set("entries",
        json(R"([{"type":"message","id":"imported-0","sessionId":"fork-session","role":"assistant","content":[{"type":"text","text":"Inherited answer"}]}])"));
    publish_projection(fork, inherited);
    const auto destination = root.root / "unified" / "fork-session";
    fs::copy(fork.transcript, destination, fs::copy_options::recursive);
    const auto initial = page(scan(request(root)));
    require(initial.records.size() == 1 && initial.records.front().total_tokens == 150,
        "fork/import transcript inherited the parent's billable usage");
    fork.publish(state);
    counters(inherited, 9, 3);
    publish_projection(fork, inherited);
    fs::remove_all(destination);
    fs::copy(fork.transcript, destination, fs::copy_options::recursive);
    const auto advanced = page(scan(request(root, initial.cache_candidate)));
    require(advanced.records.size() == 2 &&
        advanced.records.front().total_tokens + advanced.records.back().total_tokens == 162,
        "fork's own subsequent calls were subtracted or counted with imported work");
}

void configured_history_location()
{
    Store_fixture store;
    store.publish(runtime());
    publish_projection(store, projection());
    auto input = request(store);
    auto& environment = input.context.launch_environment->effective_environment;
    environment.erase("VIBE_SESSION_LOGGING__SAVE_DIR");
    fs::create_directories(store.root / "work" / "nested");
    fs::create_directories(store.root / "vibe-home");
    environment.set("VIBE_CWD", addon::Json_value(addon::path_to_utf8(store.root / "work" / "nested")));
    write(store.root / "vibe-home" / "config.toml", "[session_logging]\nsave_dir = '../..'\n");
    const auto configured = page(scan(input));
    require(configured.records.size() == 1 && configured.records.front().total_tokens == 150,
        "frozen native workdir did not resolve the user's relative history location");
    auto duplicate = request(store);
    duplicate.context.launch_environment->effective_environment.set("VIBE_CWD", *environment.find("VIBE_CWD"));
    const auto deduplicated = page(scan(duplicate));
    require(deduplicated.records.size() == 1 && deduplicated.records.front().total_tokens == 150,
        "equivalent configured history locations duplicated native usage");
}

void immutable_pagination()
{
    Store_fixture store;
    store.publish(runtime());
    publish_projection(store, projection());
    for (unsigned i = 0; i < 256; ++i) {
        const auto id = "session-" + std::to_string(i);
        Store_fixture next(id);
        next.publish(runtime(id));
        publish_projection(next, projection(id));
        fs::copy(next.transcript, store.root / "unified" / id, fs::copy_options::recursive);
    }
    auto input = request(store);
    const auto first_call = scan(input);
    const auto first = page(first_call);
    require(!first.complete && first.records.size() == 256 && first.next_cursor,
        "large history did not produce bounded usage pages");
    // Continuation must use the sealed source snapshot even when all native
    // files disappear between requests or the adapter has restarted.
    fs::rename(store.root / "unified", store.root / "unified-removed");
    input.snapshot_id = first.snapshot_id;
    input.cursor = first.next_cursor;
    const auto last_call = scan(input);
    const auto last = page(last_call);
    require(last.complete && last.records.size() == 1 && last.cache_candidate == first.cache_candidate &&
        last.scope.observed_at_ms == first.scope.observed_at_ms &&
        first.records.back().record_id < last.records.front().record_id,
        "continuation changed its source snapshot, metadata, or record ordering");
    require(scan(input).output == last_call.output, "retry changed an immutable usage page");
    input.context.working_directory += "/another-context";
    const auto changed = scan(input);
    require(!changed.success && changed.reason == "vibe_usage_snapshot_context_changed",
        "continuation accepted another frozen launcher context");
}
} // namespace

int main()
{
    try {
        cumulative_accounting();
        source_loss_and_resets();
        distinct_native_children();
        forked_history_is_not_billing();
        configured_history_location();
        immutable_pagination();
        std::cout << "Vibe native usage tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
