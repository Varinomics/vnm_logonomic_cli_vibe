#include "vibe_launcher.h"
#include "vibe_config.h"
#include "vibe_events.h"
#include "vibe_resume.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/managed_configuration.h>

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

void classify(addon::Invocation_context& context)
{
    addon::Contract_failure error;
    const auto request = addon::decode_classify_event_request(context.request().payload, &error);
    if (!request) {
        context.fail(addon::Result_status::FAILED, error.reason_code, error.detail);
        return;
    }
    context.succeed(addon::encode_classify_event_result(vibe::classify_event(*request)));
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name = "mistral-vibe-addon";
    options.adapter_version = "0.1.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
        {"managed-configuration", 1}, {"lifecycle-events", 2}, {"helper-ingress", 2},
        {"interactive-resume", 2}, {"interactive-resume", 3},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler([](addon::Invocation_context& context) {
        try {
            const auto& request = context.request();
            if (context.cancel_requested()) context.fail(addon::Result_status::CANCELLED, "cancelled");
            else if (request.capability.capability_id == "managed-configuration" && request.operation == "transform") transform(context);
            else if (request.capability.capability_id == "helper-ingress" && request.operation == "classify_event") classify(context);
            else if (request.capability.capability_id == "interactive-resume") vibe::handle_resume(context);
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
