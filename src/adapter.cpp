#include "vibe_launcher.h"

#include <logonomic_cli_addon/adapter_runtime.h>

#include <exception>
#include <utility>

namespace addon = logonomic::cli_addon;

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name = "mistral-vibe-addon";
    options.adapter_version = "0.1.0";
    options.capabilities = {
        {"presentation", 1}, {"launcher-discovery", 1}, {"launch-preparation", 3},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler([](addon::Invocation_context& context) {
        try {
            vibe::handle_launcher(context);
        }
        catch (const std::exception&) {
            // Native paths and configuration contents do not belong in a
            // protocol diagnostic. Specific failures are reported by handlers.
            context.fail(addon::Result_status::FAILED, "vibe_operation_failed");
        }
    });
    return runtime.run();
}
