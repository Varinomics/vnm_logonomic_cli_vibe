#include "vibe_events.h"

#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/limits.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

constexpr std::string_view k_session = "bc838a1c-14ec-4f08-8c0c-290d7f922c15";
constexpr std::string_view k_other_session = "8cbad950-736a-4e5a-82ac-6bf46451a0db";
constexpr std::string_view k_nonce = "1234567890abcdef1234567890abcdef";

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string native_path(std::string_view suffix)
{
    const auto path = (std::filesystem::temp_directory_path() / suffix).u8string();
    return {path.begin(), path.end()};
}

addon::Json_value native_event(std::string_view name, std::string_view session = k_session)
{
    auto result = addon::Json_value::object();
    result.set("hook_event_name", addon::Json_value(name));
    result.set("session_id", addon::Json_value(session));
    result.set("transcript_path", addon::Json_value(native_path("vibe-home/logs/session/native-session")));
    result.set("parent_session_id", addon::Json_value());
    result.set("agent_name", addon::Json_value("accept-edits"));
    return result;
}

addon::Provider_event compact(std::string_view name, const addon::Json_value& payload)
{
    auto event = vibe::compact_hook_event(name, payload, native_path("vibe-home"));
    require(event.has_value(), "native hook did not compact");
    return std::move(*event);
}

addon::Classify_event_request request(addon::Provider_event event, bool started = true)
{
    addon::Classify_event_request result;
    result.event = std::move(event);
    result.transition_nonce = k_nonce;
    if (started) {
        result.ledger.current_epoch = 1;
        result.ledger.current_transition = addon::Ledger_transition{
            addon::Transition_kind::START, std::string(k_session), std::string(k_nonce)};
    }
    addon::Contract_failure failure;
    auto decoded = addon::decode_classify_event_request(addon::encode_classify_event_request(result), &failure);
    require(decoded.has_value(), "fixture violates the host classification request contract");
    return std::move(*decoded);
}

addon::Classify_event_result classify(const addon::Classify_event_request& input)
{
    const auto result = vibe::classify_event(input);
    addon::Contract_failure failure;
    const auto decoded = addon::decode_classify_event_result(addon::encode_classify_event_result(result), &failure, 2);
    require(decoded.has_value(), "classification violates helper-ingress@2");
    for (const auto& observation : decoded->observations) {
        require(addon::lifecycle_profile_admits(vibe::lifecycle_declaration().features, observation),
            "observation exceeds the declared lifecycle features");
        require(observation.outcome_transition == addon::Outcome_transition::NONE,
            "a hook without authoritative outcome evidence reported an outcome");
        require(observation.attention_request_id.empty(), "an unpaired native notification acquired an attention ID");
    }
    return *decoded;
}

void transitions()
{
    for (const auto source : {"startup", "resume", "clear", "fork"}) {
        auto payload = native_event("session_start");
        payload.set("source", addon::Json_value(source));
        // Main forks retain ancestry and must not be mistaken for subagents.
        payload.set("parent_session_id", addon::Json_value(k_other_session));
        payload.set("vibe_home", addon::Json_value("/payload-must-not-select-home"));
        const auto result = classify(request(compact("session_start", payload), false));
        require(result.classification == addon::Classification::TRANSITION, "session start did not open a transition");
        require(result.transition == (std::string_view(source) == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START),
            "native session start source mapped to the wrong transition kind");
        const auto& observation = result.observations.at(0);
        require(observation.session_source == source, "native session source was lost");
        require(observation.provider_session_id == k_session, "session identity changed");
        require(observation.resume_record.has_value(), "session identity lacks its resume record");
        const auto& record = *observation.resume_record;
        require(record.find("version")->as_integer() == 1, "resume record version changed");
        require(record.find("provider_session_id")->as_string() == k_session, "resume record has another session");
        require(record.find("vibe_home")->as_string() == native_path("vibe-home"), "payload overrode the helper's resolved home");
        require(record.find("transcript_path")->as_string() == payload.find("transcript_path")->as_string(),
            "native transcript directory was not retained");
    }
    auto payload = native_event("session_start", k_other_session);
    payload.set("source", addon::Json_value("clear"));
    const auto replacement = classify(request(compact("session_start", payload)));
    require(replacement.classification == addon::Classification::TRANSITION, "clear did not replace the previous session identity");
    require(replacement.observations.at(0).provider_session_id == k_other_session, "clear retained the old session identity");
}

void phases_and_attention()
{
    auto payload = native_event("pre_agent");
    payload.set("turn_id", addon::Json_value("turn-native-17"));
    auto result = classify(request(compact("pre_agent", payload)));
    const auto& working = result.observations.at(0);
    require(working.phase_transition == addon::Phase_transition::WORKING && working.phase_evidence == addon::Evidence::CONFIRMED,
        "synchronous pre_agent did not confirm working");
    require(working.provider_turn_id == "turn-native-17", "native turn identity was discarded");
    for (const auto notification : {"turn_completed", "turn_failed", "approval_requested", "question_asked"}) {
        payload = native_event("notification");
        payload.set("notification_type", addon::Json_value(notification));
        // Asynchronous notifications cannot attribute themselves to a current
        // turn, even if unrelated provider payload fields carry such a value.
        payload.set("turn_id", addon::Json_value("untrusted-turn"));
        result = classify(request(compact("notification", payload)));
        const auto& observation = result.observations.at(0);
        require(observation.provider_turn_id.empty(), "background notification fabricated turn correlation");
        if (std::string_view(notification).starts_with("turn_")) {
            require(observation.phase_transition == addon::Phase_transition::AWAITING_INPUT &&
                observation.phase_evidence == addon::Evidence::INFERRED, "completion notification was not an inferred phase hint");
        }
        else {
            require(observation.attention_transition == addon::Attention_transition::REQUESTED &&
                observation.attention_evidence == addon::Evidence::INFERRED, "notification claimed confirmed attention");
        }
    }
    payload = native_event("session_end");
    payload.set("reason", addon::Json_value("clear"));
    result = classify(request(compact("session_end", payload)));
    require(result.observations.at(0).session_transition == addon::Session_transition::ENDED, "session_end did not close the session");
}

void correlation_and_unsupported_events()
{
    for (const auto name : {"session_end", "pre_agent", "notification"}) {
        auto payload = native_event(name, k_other_session);
        payload.set("reason", addon::Json_value("exit"));
        payload.set("notification_type", addon::Json_value("turn_failed"));
        auto input = request(compact(name, payload));
        require(classify(input).classification == addon::Classification::IGNORED, "late old-session event affected the replacement");
        input.ledger = {};
        require(classify(input).classification == addon::Classification::IGNORED, "unbound event created session evidence");
    }
    for (const auto name : {"post_tool", "post_agent", "turn_cancelled", "future_event"}) {
        addon::Provider_event event;
        event.name = name;
        event.body.set("session_id", addon::Json_value(k_session));
        require(classify(request(event)).observations.empty(), "unsupported native event acquired lifecycle meaning");
    }
    auto payload = native_event("notification");
    payload.set("notification_type", addon::Json_value("future_notification"));
    require(!vibe::compact_hook_event("notification", payload, native_path("vibe-home")), "unknown notification was forwarded");
    addon::Provider_event unknown;
    unknown.name = "notification";
    unknown.body = payload;
    require(classify(request(unknown)).observations.empty(), "unknown same-session notification acquired lifecycle meaning");
}

void compact_payloads()
{
    auto payload = native_event("pre_agent");
    payload.set("prompt", addon::Json_value(std::string(180000, 'p')));
    payload.set("response", addon::Json_value(std::string(180000, 'r')));
    payload.set("tool_output", addon::Json_value(std::string(180000, 't')));
    const auto event = compact("pre_agent", payload);
    require(addon::serialize_json(event.body).size() < 512, "private native content reached ingress");
    require(!event.body.find("prompt") && !event.body.find("response") && !event.body.find("tool_output"),
        "compact event retained model content");
    require(!vibe::compact_hook_event("session_end", payload, native_path("vibe-home")), "hook argument and payload disagreement was accepted");
    payload.set("session_id", addon::Json_value(std::string(257, 'a')));
    require(!vibe::compact_hook_event("pre_agent", payload, native_path("vibe-home")), "oversized provider identity was accepted");
    payload.set("session_id", addon::Json_value(std::string("invalid\0id", 10)));
    require(!vibe::compact_hook_event("pre_agent", payload, native_path("vibe-home")), "NUL provider identity was accepted");
    payload = native_event("session_start");
    payload.set("source", addon::Json_value("startup"));
    require(!vibe::compact_hook_event("session_start", payload, "relative-home"), "relative resume home was accepted");
    payload.set("transcript_path", addon::Json_value("../other-session"));
    require(!vibe::compact_hook_event("session_start", payload, native_path("vibe-home")), "relative native transcript directory was accepted");
}

void root_model_state()
{
    // Native Vibe documents this member as the active model alias, which maps
    // to a separately configured API model and provider inside Vibe.
    auto payload = native_event("session_start");
    payload.set("source", addon::Json_value("startup"));
    payload.set("model", addon::Json_value("mistral-medium-3.5"));
    auto input = request(compact("session_start", payload), false);
    require(classify(input).model_updates.empty(), "unnegotiated model data was emitted");
    input.model_state_version = 1;
    auto result = classify(input);
    require(result.classification == addon::Classification::TRANSITION &&
        result.model_updates.size() == 1 && !result.model_failure,
        "negotiated root model state was not reported with its session transition");
    const auto& model = result.model_updates.front();
    require(model.provider_session_id == k_session && model.transition_nonce == k_nonce &&
        model.sequence == 0 && model.agent_id.empty() && model.status == addon::Model_status::UNAVAILABLE &&
        model.provider.empty() && model.model.empty() && !model.reasoning_effort &&
        model.source == "vibe.session_start",
        "a native model alias became a resolved model, or the fact left its source activation");

    payload.set("session_id", addon::Json_value("native\x01session"));
    input = request(compact("session_start", payload), false);
    input.model_state_version = 1;
    result = classify(input);
    require(result.classification == addon::Classification::TRANSITION && result.model_updates.empty() &&
        result.model_failure && result.model_failure->reason_code == "vibe_model_state_invalid",
        "an unrepresentable root model state suppressed lifecycle or was emitted");

    input = request(compact("pre_agent", native_event("pre_agent")));
    input.model_state_version = 1;
    require(classify(input).model_updates.empty(), "a later hook acquired an activation from host arrival state");
    input.model_state_version = 0;
    input.model_failure = addon::Contract_failure{"unsupported_version", {}};
    result = classify(input);
    require(result.model_failure && result.model_failure->reason_code == "unsupported_version" &&
        result.observations.size() == 1, "optional request failure changed lifecycle classification");
}

void execution_directory()
{
    auto payload = native_event("session_start");
    payload.set("source", addon::Json_value("resume"));
    const auto cwd = native_path("vibe project/nested directory");
    payload.set("cwd", addon::Json_value(cwd));
    auto input = request(compact("session_start", payload), false);
    require(classify(input).execution_updates.empty(), "unnegotiated cwd was emitted");
    input.execution_context_version = 1;
    auto result = classify(input);
    require(result.execution_updates.size() == 1 && !result.execution_failure,
        "native session_start cwd was lost");
    const auto& update = result.execution_updates.front();
    require(update.provider_session_id == k_session && update.transition_nonce == k_nonce &&
        update.sequence == 0 && update.status == addon::Execution_context_status::CONFIRMED &&
        update.environment == "local" && update.cwd == cwd && update.source == "vibe.session_start.cwd",
        "joint native cwd changed its activation, path, or source");
    for (const auto invalid : {"relative/path", "/invalid\npath"}) {
        input.event.body.set("cwd", addon::Json_value(invalid));
        result = classify(input);
        require(result.classification == addon::Classification::TRANSITION && result.execution_updates.empty() &&
            result.execution_failure, "invalid optional cwd suppressed lifecycle or became confirmed");
    }
    input.event.body.set("cwd", addon::Json_value());
    result = classify(input);
    require(result.execution_updates.empty() && !result.execution_failure,
        "missing native cwd was replaced by a launch or transcript path");

    payload = native_event("pre_agent");
    payload.set("cwd", addon::Json_value(cwd));
    input = request(compact("pre_agent", payload));
    input.execution_context_version = 1;
    require(classify(input).execution_updates.empty(), "later cwd acquired a nonce from host arrival state");
    input.execution_failure = addon::Contract_failure{"unsupported_execution_version", {}};
    result = classify(input);
    require(result.observations.size() == 1 && result.execution_failure &&
        result.execution_failure->reason_code == "unsupported_execution_version",
        "optional cwd request failure affected lifecycle classification");
}

void optional_context_failure_scope()
{
    auto payload = native_event("pre_agent", k_other_session);
    auto input = request(compact("pre_agent", payload));
    input.context_failure = addon::Contract_failure{"unsupported_version", "unsupported context telemetry"};
    auto result = classify(input);
    require(result.classification == addon::Classification::IGNORED && result.context_failure &&
        result.context_failure->provider_session_id == k_other_session,
        "a delayed optional context failure borrowed the active ledger session");
    input.event.body = addon::Json_value::object();
    result = classify(input);
    require(result.context_failure && result.context_failure->provider_session_id.empty(),
        "unattributed optional context failure became another session's failure");
}

void declared_profile(const char* path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "lifecycle sidecar is missing");
    const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    const auto document = addon::parse_json(bytes, {}, nullptr);
    require(document && document->find("payload"), "invalid lifecycle sidecar");
    addon::Contract_failure failure;
    const auto declaration = addon::decode_lifecycle_declaration(*document->find("payload"), &failure);
    require(declaration.has_value(), "lifecycle sidecar violates the SDK contract");
    require(addon::encode_lifecycle_declaration(*declaration) == addon::encode_lifecycle_declaration(vibe::lifecycle_declaration()),
        "lifecycle implementation and packaged declaration disagree");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 2, "usage: vibe_events_tests <lifecycle-events@2.json>");
        declared_profile(argv[1]);
        transitions();
        phases_and_attention();
        correlation_and_unsupported_events();
        compact_payloads();
        root_model_state();
        execution_directory();
        optional_context_failure_scope();
        std::cout << "Vibe lifecycle contracts passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
