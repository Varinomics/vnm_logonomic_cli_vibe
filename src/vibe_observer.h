#pragma once

#include "vibe_context.h"

#include <logonomic_cli_addon/adapter_runtime.h>

#include <filesystem>
#include <memory>
#include <string_view>

namespace vibe {

// One instance belongs to one adapter process. SDK invocation workers perform
// bounded watch windows; this object starts no threads and owns no native session.
class Observer {
public:
    Observer();
    ~Observer();
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;

    bool reserved(std::string_view transition_nonce) const;
    // Call only for a valid authenticated helper START/RESUME classification.
    // Repeated binding is allowed only for the identical frozen context.
    bool bind(std::string_view transition_nonce, std::string_view provider_session_id,
        const std::filesystem::path& transcript);

    // Uses only the transcript already bound by an authenticated transition.
    // Reading occurs after the request, outside the observer's queue lock.
    Native_context_result read_context(std::string_view transition_nonce, std::string_view provider_session_id,
        std::optional<std::uint64_t> count_revision = std::nullopt) const;

    void handle_lifetime(logonomic::cli_addon::Invocation_context& context);
    void handle_observation(logonomic::cli_addon::Invocation_context& context);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vibe
