#include "vibe_launcher.h"
#include "vibe_accounts.h"
#include "vibe_config.h"
#include "vibe_events.h"
#include "vibe_export.h"
#include "vibe_observer.h"
#include "vibe_resume.h"
#include "vibe_tasks.h"
#include "vibe_usage.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/managed_configuration.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>

#include <exception>
#include <utility>

namespace addon = logonomic::cli_addon;

namespace {

void transform(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_managed_configuration_transform_request(context.request().payload, &error);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    try {
        auto content = vibe::updated_hooks(request->content);
        if (content.size() > addon::k_max_managed_configuration_bytes) {
            context.fail(addon::Result_status::FAILED, "payload_too_large");
            return;
        }
        context.succeed(addon::encode_managed_configuration_transform_result({request->grant, std::move(content)}));
    }
    catch (const std::runtime_error&) {
        context.fail(addon::Result_status::FAILED, "vibe_hooks_configuration_conflict",
            "The existing Vibe hook file could not be updated without changing user-owned configuration.");
    }
}

void classify(addon::Invocation_context& context, vibe::Observer& observer)
{
    addon::Contract_failure error;
    const auto request = addon::decode_classify_event_request(context.request().payload, &error);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    auto result = vibe::classify_event(*request);
    if (result.classification == addon::Classification::TRANSITION) {
        if (!observer.reserved(request->transition_nonce)) {
            context.fail(addon::Result_status::UNAVAILABLE, "vibe_correlation_not_reserved");
            return;
        }
        const auto* transcript = request->event.body.find("transcript_path");
        if (!transcript || !transcript->is_string() || result.observations.empty() ||
            !observer.bind(request->transition_nonce, result.observations.front().provider_session_id,
                addon::path_from_utf8(transcript->as_string()))) {
            context.fail(addon::Result_status::UNAVAILABLE, "vibe_correlation_context_mismatch");
            return;
        }
    }
    if (request->context_state_version == 2 && !request->context_failure && !result.observations.empty()) {
        const auto& nonce = result.classification == addon::Classification::TRANSITION ?
            request->transition_nonce : request->ledger.current_transition->nonce;
        auto native = observer.read_context(nonce, result.observations.front().provider_session_id,
            request->context_snapshot_revision);
        if (native.update) result.context_updates.push_back(std::move(*native.update));
        result.context_failure = std::move(native.failure);
    }
    context.succeed(addon::encode_classify_event_result(result));
}

} // namespace

int main()
{
    // Outlive the runtime and every invocation worker holding the handler.
    vibe::Observer observer;
    addon::Adapter_runtime_options options;
    options.adapter_name = "mistral-vibe-addon";
    options.adapter_version = LOGONOMIC_CLI_ADDON_ADAPTER_VERSION;
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3}, {"accounts", 2},
        {"managed-configuration", 1}, {"lifecycle-events", 2}, {"helper-ingress", 2},
        {"lifecycle-correlation-lifetime", 1},
        {"model-state", 1}, {"child-state", 1},
        {"structured-model-observation", 1},
        {"execution-context", 1}, {"model-context-delivery", 2},
        {"context-state", 2},
        {"profile-storage", 1}, {"usage-scan", 3}, {"usage-session", 1}, {"usage-temporal", 1},
        {"conversation-export", 2},
        {"structured-session", 3},
        {"interactive-resume", 2}, {"interactive-resume", 3},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler([&observer](addon::Invocation_context& context) {
        try {
            const auto& request = context.request();
            if (context.cancel_requested()) context.fail(addon::Result_status::CANCELLED, "cancelled");
            else if (request.capability.capability_id == "accounts") vibe::handle_accounts(context);
            else if (request.capability.capability_id == "managed-configuration" && request.operation == "transform") transform(context);
            else if (request.capability.capability_id == "helper-ingress" && request.operation == "classify_event") classify(context, observer);
            else if (request.capability.capability_id == "lifecycle-events") observer.handle_observation(context);
            else if (request.capability.capability_id == "lifecycle-correlation-lifetime") observer.handle_lifetime(context);
            else if (request.capability.capability_id == "interactive-resume") vibe::handle_resume(context);
            else if (request.capability.capability_id == "usage-scan") vibe::handle_usage_scan(context);
            else if (request.capability.capability_id == "usage-session") vibe::handle_usage_session(context);
            else if (request.capability.capability_id == "conversation-export") vibe::handle_export(context);
            else if (request.capability.capability_id == "structured-session") vibe::handle_tasks(context);
            else if (request.capability.capability_id == "profile-storage" &&
                request.operation == addon::k_profile_storage_acknowledge) addon::acknowledge_profile_storage(context);
            else vibe::handle_launcher(context);
        }
        catch (const std::exception&) {
            // Native paths and configuration contents do not belong in a
            // protocol diagnostic. Specific failures are reported by handlers.
            context.fail(addon::Result_status::FAILED, "vibe_operation_failed");
        }
    });
    return runtime.run();
}
