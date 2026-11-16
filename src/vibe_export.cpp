#include "vibe_export.h"

#include "vibe_launcher.h"
#include "vibe_resume.h"
#include "vibe_store.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>

#include <map>
#include <stdexcept>

namespace vibe {
namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

class Export_error final : public std::runtime_error
{
public:
    explicit Export_error(std::string_view reason) : std::runtime_error(std::string(reason)) {}
};

void require(bool condition, std::string_view reason = "vibe_export_history_invalid")
{
    if (!condition) {
        throw Export_error(reason);
    }
}

const std::string* optional_text(const addon::Json_value& value, std::string_view key)
{
    const auto* member = value.find(key);
    return member && member->is_string() ? &member->as_string() : nullptr;
}

const std::string& text(const addon::Json_value& value, std::string_view key)
{
    const auto* member = optional_text(value, key);
    require(member != nullptr);
    return *member;
}

addon::Conversation_block block(addon::Conversation_block_kind kind, std::string value,
    std::string label = {}, bool compact_visible = true)
{
    return {kind, std::move(value), std::move(label), compact_visible};
}

void add_reference(std::string& marker, const addon::Json_value& value, std::string_view member)
{
    if (const auto* reference = optional_text(value, member); reference && !reference->empty()) {
        marker += "\n";
        marker += *reference;
    }
}

addon::Conversation_block content_block(const addon::Json_value& content)
{
    const auto& kind = text(content, "type");
    if (kind == "text") {
        return block(addon::Conversation_block_kind::TEXT, text(content, "text"));
    }
    if (kind == "image" || kind == "audio") {
        auto marker = kind == "image" ? std::string("Image attachment; binary content not copied.") :
            std::string("Audio attachment; binary content not copied.");
        add_reference(marker, content, "mimeType");
        if (const auto* attachment = content.find("attachment"); attachment && attachment->is_object()) {
            add_reference(marker, *attachment, "alias");
            add_reference(marker, *attachment, "mimeType");
            if (const auto* source = attachment->find("source"); source && source->is_object()) {
                add_reference(marker, *source, "uri");
                add_reference(marker, *source, "path");
            }
        }
        add_reference(marker, content, "uri");
        return block(addon::Conversation_block_kind::ATTACHMENT, std::move(marker), kind);
    }
    if (kind == "resource") {
        const auto* resource = content.find("resource");
        require(resource && resource->is_object());
        const auto* declared_kind = optional_text(*resource, "kind");
        const std::string resource_kind = declared_kind ? *declared_kind :
            optional_text(*resource, "text") ? "text" : optional_text(*resource, "blob") ? "blob" : "";
        require(resource_kind == "text" || resource_kind == "blob" || resource_kind == "link",
            "vibe_export_content_unsupported");
        std::string marker = "Resource attachment";
        add_reference(marker, *resource, "uri");
        add_reference(marker, *resource, "name");
        add_reference(marker, *resource, "title");
        add_reference(marker, *resource, "mediaType");
        add_reference(marker, *resource, "mimeType");
        if (resource_kind == "text") {
            marker += "\n\n" + text(*resource, "text");
        }
        else {
            marker += resource_kind == "link" ? "\nReferenced content not fetched." : "\nBinary content not copied.";
        }
        return block(addon::Conversation_block_kind::ATTACHMENT, std::move(marker), "Resource");
    }
    if (kind == "resource_link") {
        std::string marker = "Resource attachment; referenced content not fetched.";
        add_reference(marker, content, "uri");
        add_reference(marker, content, "name");
        add_reference(marker, content, "mimeType");
        return block(addon::Conversation_block_kind::ATTACHMENT, std::move(marker), "Resource");
    }
    throw Export_error("vibe_export_content_unsupported");
}

addon::Conversation_message message_entry(const addon::Json_value& entry)
{
    addon::Conversation_message message;
    message.role = text(entry, "role");
    require(!message.role.empty());
    const auto* content = entry.find("content");
    require(content && content->is_array());
    const auto* outcome = entry.find("outcome");
    const auto* outcome_kind = outcome ? optional_text(*outcome, "type") : nullptr;
    const bool discarded = outcome_kind && *outcome_kind == "discarded";
    if (discarded) {
        std::string label = "Discarded assistant content";
        if (const auto* cause = optional_text(*outcome, "cause")) {
            label += " (" + *cause + ")";
        }
        message.content.push_back(block(addon::Conversation_block_kind::TEXT, std::move(label), "Outcome", false));
    }
    for (const auto& part : content->as_array()) {
        auto rendered = content_block(part);
        rendered.compact_visible = !discarded;
        message.content.push_back(std::move(rendered));
    }
    // The public history does not retain a final/commentary channel for every
    // assistant message. Keep its phase unknown instead of guessing by position.
    return message;
}

addon::Conversation_message effect_entry(const addon::Json_value& entry)
{
    addon::Conversation_message message;
    message.role = "assistant";
    const auto* detail = entry.find("detail");
    const auto* state = entry.find("state");
    require(detail && detail->is_object() && state && state->is_object());
    const auto* tool = optional_text(*detail, "toolName");
    const auto* title = optional_text(entry, "title");
    const auto label = (tool ? *tool : title ? *title : "Tool") + " " + text(entry, "id");
    if (const auto* input = detail->find("input")) {
        message.content.push_back(block(addon::Conversation_block_kind::TOOL_CALL,
            addon::serialize_json_canonical(*input), label, false));
    }
    else {
        message.content.push_back(block(addon::Conversation_block_kind::TOOL_CALL,
            addon::serialize_json_canonical(*detail), label, false));
    }
    const auto* output = state->find("output");
    const auto* content = output ? output->find("content") : nullptr;
    if (content && content->is_array()) {
        for (const auto& part : content->as_array()) {
            auto rendered = content_block(part);
            if (rendered.kind == addon::Conversation_block_kind::TEXT) {
                rendered.kind = addon::Conversation_block_kind::TOOL_RESULT;
            }
            rendered.label = label;
            rendered.compact_visible = false;
            message.content.push_back(std::move(rendered));
        }
    }
    else if (const auto* output_text = optional_text(*state, "outputText"); output_text && !output_text->empty()) {
        message.content.push_back(block(addon::Conversation_block_kind::TOOL_RESULT, *output_text, label, false));
    }
    else if (output && !output->is_null()) {
        message.content.push_back(block(addon::Conversation_block_kind::TOOL_RESULT,
            addon::serialize_json_canonical(*output), label, false));
    }
    const auto& status = text(*state, "status");
    if (status != "completed") {
        std::string outcome = status;
        if (const auto* error = state->find("error")) {
            outcome += "\n" + addon::serialize_json_canonical(*error);
        }
        add_reference(outcome, *state, "reason");
        message.content.push_back(block(addon::Conversation_block_kind::TOOL_RESULT,
            std::move(outcome), label, false));
    }
    return message;
}

addon::Conversation_message render_entry(const addon::Json_value& entry)
{
    const auto& kind = text(entry, "type");
    if (kind == "message") {
        return message_entry(entry);
    }
    if (kind == "effect") {
        return effect_entry(entry);
    }
    addon::Conversation_message message;
    message.role = kind == "reasoning" ? "assistant" : "system";
    if (kind == "reasoning") {
        std::string body = text(entry, "text");
        if (const auto* summaries = entry.find("summary"); summaries && summaries->is_array()) {
            for (const auto& summary : summaries->as_array()) {
                require(summary.is_string());
                if (!body.empty()) {
                    body += "\n";
                }
                body += summary.as_string();
            }
        }
        message.content.push_back(block(addon::Conversation_block_kind::TEXT, std::move(body), "Reasoning", false));
    }
    else if (kind == "checkpoint" || kind == "notice") {
        std::string body = text(entry, "message");
        const auto* details = entry.find(kind == "checkpoint" ? "details" : "detail");
        if (details && !details->is_null()) {
            body += "\n" + addon::serialize_json_canonical(*details);
        }
        message.content.push_back(block(addon::Conversation_block_kind::TEXT, std::move(body), kind, false));
    }
    else if (kind == "callback") {
        const auto* detail = entry.find("detail");
        const auto* state = entry.find("state");
        require(detail && detail->is_object() && state && state->is_object());
        const auto* callback_kind = optional_text(*detail, "kind");
        const bool user_input = callback_kind && *callback_kind == "user_input";
        message.role = user_input ? "user" : "system";
        const auto body = text(entry, "title") + "\n" + addon::serialize_json_canonical(*detail) +
            "\n" + addon::serialize_json_canonical(*state);
        message.content.push_back(block(addon::Conversation_block_kind::TEXT, body,
            user_input ? "Question and response" : "Approval", user_input));
    }
    else {
        throw Export_error("vibe_export_entry_unsupported");
    }
    return message;
}

fs::path locate(const addon::Conversation_export_request& request)
{
    require(!request.account_context, "vibe_export_account_unavailable");
    const auto record = request.resume_record ? decode_resume_record(*request.resume_record) : std::nullopt;
    require(record && record->session_id == request.provider_session_id, "vibe_export_source_unavailable");
    const auto environment = addon::environment_pairs(request.launch_environment.effective_environment);
    // Export can outlive the original working directory. The retained source
    // locator and frozen home bind identity; resume eligibility is irrelevant.
    const auto home = resolve_home(environment, addon::path_to_utf8(record->home));
    std::error_code error;
    const auto recorded_home = fs::weakly_canonical(record->home, error);
    require(home && !error && *home == recorded_home, "vibe_export_home_mismatch");
    return record->transcript;
}

} // namespace

std::optional<addon::Conversation_export_result> export_native_conversation(
    const addon::Conversation_export_request& request,
    const std::function<bool()>& cancelled, addon::Contract_failure* failure)
{
    try {
        require(!cancelled || !cancelled(), "cancelled");
        const auto transcript = locate(request);
        const auto native = read_native_snapshot(transcript, request.provider_session_id);
        require(native.snapshot.has_value(), native.reason);
        const auto projection = read_native_projection(transcript, *native.snapshot);
        require(projection.projection.has_value(), projection.reason);
        require(!projection.journal_pending, "vibe_export_journal_pending");
        const auto* entries = projection.projection->find("snapshot")->find("history")->find("entries");
        addon::Conversation_writer writer(request, cancelled);
        if (!writer.open(failure)) {
            return std::nullopt;
        }
        std::map<std::string, const addon::Json_value*> seen;
        bool incomplete_tail = false;
        for (const auto& entry : entries->as_array()) {
            require(!cancelled || !cancelled(), "cancelled");
            require(text(entry, "sessionId") == request.provider_session_id, "vibe_export_session_mismatch");
            const auto& id = text(entry, "id");
            require(!id.empty());
            const auto prior = seen.find(id);
            if (prior != seen.end()) {
                require(*prior->second == entry, "vibe_export_duplicate_conflict");
                continue;
            }
            seen.emplace(id, &entry);
            const auto& generation = text(entry, "generationStatus");
            if (generation == "in_progress") {
                incomplete_tail = true;
                continue;
            }
            require(generation == "completed", "vibe_export_generation_unsupported");
            require(!incomplete_tail, "vibe_export_unsettled_history");
            if (!writer.append(render_entry(entry), failure)) {
                return std::nullopt;
            }
        }
        return writer.finish(incomplete_tail, failure);
    }
    catch (const Export_error& error) {
        if (failure) {
            *failure = addon::Contract_failure{error.what(), {}};
        }
    }
    catch (const std::exception&) {
        if (failure) {
            *failure = addon::Contract_failure{"vibe_export_failed", {}};
        }
    }
    return std::nullopt;
}

void handle_export(addon::Invocation_context& context)
{
    addon::handle_conversation_export(context,
        [](const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
            addon::Contract_failure* failure)
        {
            auto result = export_native_conversation(request, cancelled, failure);
            if (!result && failure) {
                const auto& reason = failure->reason_code;
                if (reason == "vibe_export_journal_pending" || reason == "vibe_export_unsettled_history" ||
                    reason == "vibe_export_source_unavailable" || reason == "vibe_export_home_mismatch" ||
                    reason == "vibe_export_account_unavailable")
                {
                    failure->detail = reason;
                    failure->reason_code = "conversation_source_unavailable";
                }
                else if (reason == "vibe_export_content_unsupported" || reason == "vibe_export_entry_unsupported" ||
                    reason == "vibe_export_generation_unsupported")
                {
                    failure->detail = reason;
                    failure->reason_code = "conversation_content_unavailable";
                }
            }
            return result;
        });
}

} // namespace vibe
