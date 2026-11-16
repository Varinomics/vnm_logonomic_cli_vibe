#include "vibe_observer.h"
#include "vibe_store_fixture.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/package_source.h>

#include <functional>
#include <iostream>

namespace {
using namespace vibe_test;
constexpr std::string_view k_nonce = "0123456789abcdef0123456789abcdef";
constexpr std::string_view k_other_nonce = "fedcba9876543210fedcba9876543210";

class Invocation final : public addon::Invocation_context {
public:
    addon::Invoke_request input;
    addon::Json_value output;
    bool done = false;
    bool success = false;
    bool cancelled = false;
    std::string reason;
    unsigned waits = 0;
    unsigned stop_after = 1;
    std::function<void(unsigned)> on_wait;
    std::vector<std::uint64_t> progress;

    const addon::Invoke_request& request() const override { return input; }
    bool cancel_requested() const override { return cancelled; }
    bool wait_for_cancel(int) override
    {
        ++waits;
        if (on_wait) on_wait(waits);
        if (waits >= stop_after) cancelled = true;
        return cancelled;
    }
    void report_progress(std::uint64_t completed, std::optional<std::uint64_t> total, std::string note) override
    {
        require(!total, "observer progress invented a total");
        require(note == addon::observe_progress_note(completed), "progress carried data instead of a count");
        progress.push_back(completed);
    }
    void succeed(addon::Json_value result) override { output = std::move(result); done = success = true; }
    void fail(addon::Result_status, std::string code, std::string) override { reason = std::move(code); done = true; }
    bool finished() const override { return done; }
};

void reserve(vibe::Observer& observer, std::string_view nonce = k_nonce)
{
    Invocation call;
    call.input.operation = "reserve";
    call.input.payload = addon::encode_lifecycle_correlation_reserve_request({{std::string(nonce)}});
    observer.handle_lifetime(call);
    require(call.success && call.output == addon::Json_value::object(), "correlation reserve failed");
}

Invocation observation(const Store_fixture& store, std::string watch_cursor = {},
    std::string_view nonce = k_nonce, bool models = false)
{
    Invocation call;
    call.input.operation = "observe";
    addon::Observe_request request;
    request.session.working_directory = addon::path_to_utf8(store.root);
    request.session.launcher_argv = {"vibe"};
    request.session.provider_session_id = "root-session";
    request.resume_cursor = std::move(watch_cursor);
    request.window_ms = 60000;
    request.correlation = addon::Session_correlation{std::string(nonce)};
    request.execution_model_version = models ? 1 : 0;
    call.input.payload = addon::encode_observe_request(request);
    return call;
}

Invocation poll(vibe::Observer& observer, std::string consumption = {}, std::size_t limit = 16,
    std::string_view nonce = k_nonce, bool models = false)
{
    Invocation call;
    call.input.operation = "poll_observations";
    addon::Poll_observations_request request;
    request.resume_cursor = std::move(consumption);
    request.limit = limit;
    request.correlation = addon::Session_correlation{std::string(nonce)};
    request.execution_model_version = models ? 1 : 0;
    call.input.payload = addon::encode_poll_observations_request(request);
    observer.handle_observation(call);
    return call;
}

addon::Poll_observations_result page(const Invocation& call, std::size_t limit = 16)
{
    require(call.success, "poll did not succeed");
    addon::Contract_failure error;
    const auto result = addon::decode_poll_observations_result(call.output, limit, &error);
    require(result.has_value(), "observer returned an invalid SDK page");
    return *result;
}

void bind(vibe::Observer& observer, const Store_fixture& store)
{
    require(!observer.bind(k_nonce, "root-session", store.transcript), "unreserved context was bound");
    reserve(observer);
    require(observer.bind(k_nonce, "root-session", store.transcript), "reserved context did not bind");
    reserve(observer);
    require(observer.bind(k_nonce, "root-session", store.transcript), "repeated reserve replaced frozen context");
    require(!observer.bind(k_nonce, "different-session", store.transcript.parent_path() / "different-session"),
        "bound nonce was moved to another session");
}

void child_runs_and_cursors()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    set_child(state, child("reserved"));
    store.publish(state);
    bind(observer, store);
    auto watch = observation(store);
    const std::vector<std::pair<std::string, std::uint64_t>> changes = {
        {"running", 1}, {"idle", 1}, {"running", 2}, {"interrupted", 2},
        {"running", 3}, {"turn_failed", 3}, {"running", 4}, {"deleting_running", 4}, {"tombstone", 1}};
    watch.stop_after = static_cast<unsigned>(changes.size() + 1);
    watch.on_wait = [&](unsigned count) {
        if (count <= changes.size()) {
            const auto& [status, generation] = changes[count - 1];
            set_child(state, child(status, generation));
            store.publish(state);
        }
    };
    observer.handle_observation(watch);
    require(watch.success && !watch.progress.empty() && watch.progress.back() == 8, "child run changes were lost");
    const auto watch_result = addon::decode_observe_result(watch.output, nullptr);
    require(watch_result.has_value(), "watch result violates SDK contract");

    const auto first_call = poll(observer, {}, 2);
    const auto first = page(first_call, 2);
    const auto repeated = poll(observer, {}, 2);
    require(repeated.output == first_call.output, "retry consumed an unacknowledged page");
    require(first.more && first.observations.size() == 2, "bounded page did not retain remaining observations");
    require(first.observations[0].agent_id == "child-one:turn:1" &&
        first.observations[1].agent_id == "child-one:turn:1", "reserved and completed first run lost their shared identity");
    require(first.observations[1].provider_event_name == "vibe_subagent_idle", "completed child lost its observed native state");
    std::vector<addon::Lifecycle_observation> all = first.observations;
    auto next_cursor = first.resume_cursor;
    for (;;) {
        const auto next = page(poll(observer, next_cursor, 2), 2);
        all.insert(all.end(), next.observations.begin(), next.observations.end());
        next_cursor = next.resume_cursor;
        if (!next.more) break;
    }
    require(all.size() == 8, "acknowledgement dropped or duplicated child observations");
    for (std::size_t index = 0; index < all.size(); ++index) {
        const auto& item = all[index];
        require(item.agent_id == "child-one:turn:" + std::to_string(index / 2 + 1), "later run reused an earlier stopped span");
        require(item.provider_turn_id.empty() && item.phase_transition == addon::Phase_transition::NONE &&
            item.outcome_transition == addon::Outcome_transition::NONE && item.attention_transition == addon::Attention_transition::NONE &&
            item.session_transition == addon::Session_transition::NONE, "child activity changed root lifecycle state");
        require(item.provider_sequence == std::to_string(index + 1), "observation order is unstable");
    }
    require(all[3].provider_event_name == "vibe_subagent_interrupted" && all[5].provider_event_name == "vibe_subagent_turn_failed",
        "interrupted and failed children were reported as success");
    const auto empty = page(poll(observer, next_cursor));
    require(empty.observations.empty() && !empty.more, "acknowledged observations stayed queued");
    require(!poll(observer).success, "expired consumption cursor silently rewound the queue");
    require(!poll(observer, watch_result->resume_cursor).success, "watch cursor acknowledged a poll page");
    require(!poll(observer, "v1:" + std::string(k_nonce) + ":p:999").success, "cursor skipped undelivered observations");
    auto next_watch = observation(store, watch_result->resume_cursor);
    observer.handle_observation(next_watch);
    require(next_watch.success && next_watch.progress.empty(), "unchanged store repeated child transitions");
    reserve(observer, k_other_nonce);
    require(observer.bind(k_other_nonce, "root-session", store.transcript), "second independent context did not bind");
    require(!poll(observer, next_cursor, 16, k_other_nonce).success, "cursor crossed authenticated correlations");
}

void state_gaps_and_forks()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    state.find("identity")->set("kind", addon::Json_value("fork"));
    state.find("identity")->set("root_session_id", addon::Json_value("ancestor"));
    state.find("identity")->set("parent_session_id", addon::Json_value("fork-source"));
    state.find("session_metadata")->set("root_session_id", addon::Json_value("ancestor"));
    state.find("session_metadata")->set("parent_session_id", addon::Json_value("fork-source"));
    set_child(state, child("idle"));
    store.publish(state);
    bind(observer, store);
    auto initial = observation(store);
    observer.handle_observation(initial);
    require(initial.success && page(poll(observer)).observations.empty(), "historical idle child was falsely started");
    set_child(state, child("running", 2));
    store.publish(state);
    auto watch = observation(store);
    watch.stop_after = 3;
    watch.on_wait = [&](unsigned count) {
        if (count == 1) set_child(state, child("running", 3));
        if (count == 2) set_child(state, child("tombstone"));
        if (count < 3) store.publish(state);
    };
    observer.handle_observation(watch);
    const auto result = page(poll(observer));
    require(result.observations.size() == 4, "skipped idle state lost separate child executions");
    require(result.observations[0].provider_session_id == "root-session", "fork child was attributed to ancestor conversation");
    require(result.observations[1].provider_event_name == "vibe_subagent_turn_advanced", "unobserved turn outcome was invented");
    require(result.observations[3].agent_id == "child-one:turn:3", "tombstone stopped the wrong child run");
}

void unavailable_and_retirement()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    set_child(state, child());
    store.publish(state);
    bind(observer, store);
    auto watch = observation(store);
    watch.stop_after = 10;
    watch.on_wait = [&](unsigned count) {
        if (count == 1) write(store.transcript / "CURRENT", "partial publication");
    };
    observer.handle_observation(watch);
    require(!watch.success && watch.reason == "vibe_store_current_unavailable", "broken store was silently treated as an empty graph");
    const auto retained = page(poll(observer));
    require(retained.observations.size() == 1 && retained.observations[0].subagent_transition == addon::Subagent_transition::STARTED,
        "read failure fabricated a stopped child");

    store.publish(state);
    auto active = observation(store);
    active.stop_after = 3;
    active.on_wait = [&](unsigned count) {
        if (count != 1) return;
        Invocation retire;
        retire.input.operation = "retire";
        retire.input.payload = addon::encode_lifecycle_correlation_retire_request({
            {std::string(k_nonce)}, addon::Lifecycle_correlation_retire_disposition::ABANDONED, std::nullopt});
        observer.handle_lifetime(retire);
        require(retire.success, "active observer could not be retired");
        observer.handle_lifetime(retire);
        require(retire.success, "repeated retirement was not idempotent");
    };
    observer.handle_observation(active);
    require(active.done && active.waits == 1 && !observer.reserved(k_nonce), "retirement did not end the held watch");
    require(!poll(observer).success, "retired queue remained reachable");
}

void labels()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    std::string name;
    for (unsigned i = 0; i != 200; ++i) name += "🔍";
    set_child(state, child("running", 1, "child-one", name));
    store.publish(state);
    bind(observer, store);
    auto watch = observation(store);
    observer.handle_observation(watch);
    const auto result = page(poll(observer));
    require(result.observations.size() == 1 && result.observations[0].agent_label.size() == 512,
        "continuous child name bound split UTF-8 or exceeded UTF-16 units");
}

void fast_runs_and_regression()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    store.publish(state);
    bind(observer, store);
    // This child starts and finishes after the gated baseline but before the
    // first watch sample. The native terminal outcome proves its whole run.
    set_child(state, child("idle"));
    store.publish(state);
    auto watch = observation(store);
    observer.handle_observation(watch);
    auto result = page(poll(observer));
    require(result.observations.size() == 2 && result.observations[0].agent_id == "child-one:turn:1" &&
        result.observations[0].provider_event_name == "vibe_subagent_recorded_run" &&
        result.observations[1].subagent_transition == addon::Subagent_transition::STOPPED,
        "fast completed run after the authenticated baseline was lost");
    page(poll(observer, result.resume_cursor));
    set_child(state, child("interrupted", 2));
    store.publish(state);
    watch = observation(store);
    observer.handle_observation(watch);
    result = page(poll(observer, result.resume_cursor));
    require(result.observations.size() == 2 && result.observations[0].agent_id == "child-one:turn:2" &&
        result.observations[1].provider_event_name == "vibe_subagent_interrupted", "fast later run reused an old span");
    set_child(state, child("running", 1));
    store.publish(state);
    watch = observation(store);
    observer.handle_observation(watch);
    require(!watch.success && watch.reason == "vibe_child_generation_regressed", "old native child state resurrected a stopped run");
}

void selected_model_changes()
{
    Store_fixture store;
    vibe::Observer observer;
    auto state = runtime();
    store.publish(state);
    bind(observer, store);
    auto watch = observation(store, {}, k_nonce, true);
    observer.handle_observation(watch);
    require(page(poll(observer, {}, 16, k_nonce, true)).model_updates.empty(),
        "a possibly historical startup pin was reported as a live selection");

    watch = observation(store, {}, k_nonce, true);
    watch.stop_after = 4;
    watch.on_wait = [&](unsigned count) {
        if (count == 1) {
            state.find("session_metadata")->set("active_model", addon::Json_value("provider/model-b"));
        }
        if (count == 2) {
            set_child(state, child());
        }
        if (count == 3) {
            state.find("session_metadata")->set("active_model", addon::Json_value("provider/model-c"));
        }
        if (count < 4) {
            store.publish(state);
        }
    };
    observer.handle_observation(watch);
    require(watch.success, "live model observation failed");
    const auto first_call = poll(observer, {}, 1, k_nonce, true);
    const auto first = page(first_call, 1);
    require(first.model_updates.size() == 1 && first.observations.empty() && first.more,
        "model-only page did not use the replayable consumption cursor");
    require(first_call.output == poll(observer, {}, 1, k_nonce, true).output,
        "unacknowledged model page could not be retried");
    require(first.model_updates.front().model == "provider/model-b" &&
        first.model_updates.front().provider_session_id == "root-session" &&
        first.model_updates.front().agent_id.empty(), "root model labelled another subject");
    const auto second = page(poll(observer, first.resume_cursor, 1, k_nonce, true), 1);
    require(second.observations.size() == 1 && second.model_updates.empty() && second.more,
        "root model was copied to a child without native child-model evidence");
    const auto third = page(poll(observer, second.resume_cursor, 1, k_nonce, true), 1);
    require(third.model_updates.size() == 1 && third.model_updates.front().model == "provider/model-c" && !third.more,
        "later selected model was lost behind a child page");
    require(page(poll(observer, third.resume_cursor, 1, k_nonce, true), 1).model_updates.empty(),
        "acknowledged model update remained queued");

    state.find("session_metadata")->set("active_model", addon::Json_value("provider/model-d"));
    store.publish(state);
    watch = observation(store, {}, k_nonce, true);
    observer.handle_observation(watch);
    require(page(poll(observer, third.resume_cursor)).model_updates.empty(),
        "model data was emitted without result negotiation");
}
} // namespace

int main()
{
    try {
        child_runs_and_cursors();
        state_gaps_and_forks();
        unavailable_and_retirement();
        labels();
        fast_runs_and_regression();
        selected_model_changes();
        std::cout << "Vibe continuous observer tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
