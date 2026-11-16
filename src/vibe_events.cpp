#include "vibe_events.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/limits.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vibe {
namespace addon = logonomic::cli_addon;

namespace {

std::optional<std::string_view> text(
    const addon::Json_value& value, std::string_view key, std::size_t limit)
{
    const auto* member = value.find(key);
    if (!member || !member->is_string()) {
        return std::nullopt;
    }
    const auto& result = member->as_string();
    if (result.empty() || result.size() > limit || result.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return result;
}

bool one_of(std::string_view value, std::initializer_list<std::string_view> choices)
{
    for (const auto choice : choices) {
        if (value == choice) {
            return true;
        }
    }
    return false;
}

bool absolute_path(std::string_view value)
{
    return value.size() <= 4096 && addon::normalized_resume_directory(value).has_value();
}

std::optional<addon::Json_value> resume_record(const addon::Json_value& body)
{
    const auto session_id = text(body, "session_id", addon::limits::k_max_lifecycle_identity_bytes);
    const auto home = text(body, "vibe_home", 4096);
    const auto transcript = text(body, "transcript_path", 4096);
    if (!session_id || !home || !transcript || !absolute_path(*home) || !absolute_path(*transcript)) {
        return std::nullopt;
    }
    auto result = addon::Json_value::object();
    result.set("version", addon::Json_value(1));
    result.set("provider_session_id", addon::Json_value(*session_id));
    result.set("vibe_home", addon::Json_value(*home));
    result.set("transcript_path", addon::Json_value(*transcript));
    if (addon::serialize_json_canonical(result).size() > addon::limits::k_max_resume_record_bytes) {
        return std::nullopt;
    }
    return result;
}

} // namespace

addon::Lifecycle_declaration lifecycle_declaration()
{
    addon::Lifecycle_declaration declaration;
    declaration.profile_id = "mistral-vibe-hooks";
    declaration.features.root_turn_start = addon::Feature_support::SUPPORTED;
    declaration.features.root_turn_completion = addon::Feature_support::SUPPORTED;
    declaration.features.confirmed_phase_status = addon::Feature_support::SUPPORTED;
    declaration.features.attention = addon::Feature_support::SUPPORTED;
    declaration.features.subagents = addon::Feature_support::SUPPORTED;
    declaration.observation = addon::Observation_mode::CONTINUOUS;
    return declaration;
}

std::optional<addon::Provider_event> compact_hook_event(
    std::string_view hook_name, const addon::Json_value& payload, std::string_view vibe_home)
{
    if (!payload.is_object() || !one_of(hook_name, {"session_start", "session_end", "pre_agent", "notification"})) {
        return std::nullopt;
    }
    const auto declared_name = text(payload, "hook_event_name", 256);
    const auto session_id = text(payload, "session_id", addon::limits::k_max_lifecycle_identity_bytes);
    if (!declared_name || *declared_name != hook_name || !session_id) {
        return std::nullopt;
    }
    addon::Provider_event event;
    event.name = hook_name;
    event.body.set("session_id", addon::Json_value(*session_id));
    if (hook_name == "session_start") {
        const auto source = text(payload, "source", 160);
        const auto transcript = text(payload, "transcript_path", 4096);
        if (!source || !one_of(*source, {"startup", "resume", "clear", "fork"}) ||
            !transcript || !absolute_path(*transcript) || !absolute_path(vibe_home))
        {
            return std::nullopt;
        }
        event.body.set("source", addon::Json_value(*source));
        event.body.set("vibe_home", addon::Json_value(vibe_home));
        event.body.set("transcript_path", addon::Json_value(*transcript));
        if (const auto model = text(payload, "model", 256)) {
            event.body.set("model", addon::Json_value(*model));
        }
        if (const auto cwd = text(payload, "cwd", 4096)) {
            event.body.set("cwd", addon::Json_value(*cwd));
        }
        if (!resume_record(event.body)) {
            return std::nullopt;
        }
    }
    else
    if (hook_name == "session_end") {
        const auto reason = text(payload, "reason", 160);
        if (!reason || !one_of(*reason, {"exit", "clear", "other"})) {
            return std::nullopt;
        }
        event.body.set("reason", addon::Json_value(*reason));
    }
    else
    if (hook_name == "pre_agent") {
        // The configured match="main" filters actual subagents. A non-null
        // parent_session_id also occurs on a main fork, so it is not a filter.
        const auto turn_id = text(payload, "turn_id", addon::limits::k_max_lifecycle_identity_bytes);
        if (turn_id) {
            event.body.set("turn_id", addon::Json_value(*turn_id));
        }
    }
    else {
        const auto notification = text(payload, "notification_type", 160);
        if (!notification || !one_of(*notification, {
                "approval_requested", "question_asked", "turn_completed", "turn_failed"}))
        {
            return std::nullopt;
        }
        event.body.set("notification_type", addon::Json_value(*notification));
    }
    return event;
}

addon::Classify_event_result classify_event(const addon::Classify_event_request& request)
{
    addon::Classify_event_result result;
    result.model_failure = request.model_failure;
    result.execution_failure = request.execution_failure;
    const auto& event = request.event;
    const auto session_id = text(event.body, "session_id", addon::limits::k_max_lifecycle_identity_bytes);
    if (!session_id) {
        return result;
    }
    addon::Lifecycle_observation observation;
    observation.provider_event_name = event.name;
    observation.provider_session_id = *session_id;
    if (event.name == "session_start") {
        const auto source = text(event.body, "source", 160);
        auto record = resume_record(event.body);
        if (!source || !one_of(*source, {"startup", "resume", "clear", "fork"}) || !record) {
            return result;
        }
        result.classification = addon::Classification::TRANSITION;
        result.transition = *source == "resume" ? addon::Transition_kind::RESUME : addon::Transition_kind::START;
        observation.session_transition = addon::Session_transition::STARTED;
        observation.session_source = *source;
        observation.resume_record = std::move(record);
        // The native session_start payload captures the actual default cwd.
        // Joint identity/context admission uses this delivery's source nonce;
        // later hooks cannot attribute delayed cwd facts to an activation.
        if (request.execution_context_version == 1) {
            if (const auto cwd = text(event.body, "cwd", 4096)) {
                addon::Execution_context_update update{
                    std::string(*session_id), request.transition_nonce, 0,
                    addon::Execution_context_status::CONFIRMED, "local", std::string(*cwd),
                    "vibe.session_start.cwd"};
                if (addon::is_valid_execution_context_update(update)) {
                    result.execution_updates.push_back(std::move(update));
                }
                else {
                    result.execution_failure = addon::Contract_failure{"vibe_execution_directory_invalid", {}};
                }
            }
        }
        if (request.execution_model_version == 1) {
            if (const auto model = text(event.body, "model", 256)) {
                addon::Execution_model_update update{
                    std::string(*session_id), {}, std::string(*model), addon::Execution_model_source::SESSION};
                if (addon::is_valid_execution_model_update(update)) {
                    result.model_updates.push_back(std::move(update));
                }
                else {
                    result.model_failure = addon::Contract_failure{"vibe_model_invalid", {}};
                }
            }
        }
    }
    else {
        // Background notifications and session_end can arrive after a clear or
        // resume. The host ledger, not filesystem recency, owns this identity.
        if (!request.ledger.current_transition || request.ledger.current_transition->provider_session_id != *session_id) {
            return result;
        }
        if (event.name == "session_end") {
            const auto reason = text(event.body, "reason", 160);
            if (!reason || !one_of(*reason, {"exit", "clear", "other"})) {
                return result;
            }
            observation.session_transition = addon::Session_transition::ENDED;
            observation.session_end_reason = *reason;
        }
        else
        if (event.name == "pre_agent") {
            observation.phase_transition = addon::Phase_transition::WORKING;
            observation.phase_evidence = addon::Evidence::CONFIRMED;
            if (const auto turn_id = text(event.body, "turn_id", addon::limits::k_max_lifecycle_identity_bytes)) {
                observation.provider_turn_id = *turn_id;
            }
        }
        else
        if (event.name == "notification") {
            const auto notification = text(event.body, "notification_type", 160);
            if (!notification) {
                return result;
            }
            // Native notifications have neither a turn ID nor an ordering
            // guarantee. They support a best-effort phase/attention hint, never
            // a confirmed outcome, permission pair, or fabricated turn identity.
            if (*notification == "turn_completed" || *notification == "turn_failed") {
                observation.phase_transition = addon::Phase_transition::AWAITING_INPUT;
                observation.phase_evidence = addon::Evidence::INFERRED;
            }
            else
            if (*notification == "approval_requested" || *notification == "question_asked") {
                observation.attention_transition = addon::Attention_transition::REQUESTED;
                observation.attention_evidence = addon::Evidence::INFERRED;
            }
            else {
                return result;
            }
        }
        else {
            return result;
        }
        result.classification = addon::Classification::EVENT;
    }
    result.observations.push_back(std::move(observation));
    return result;
}

} // namespace vibe
