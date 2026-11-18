#include "vibe_observer.h"
#include "vibe_store.h"

#include <logonomic_cli_addon/lifecycle_correlation_lifetime.h>
#include <logonomic_cli_addon/lifecycle_events.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace vibe {
namespace addon = logonomic::cli_addon;

namespace {

constexpr std::size_t k_max_correlations = 128;
constexpr std::size_t k_max_queued_observations = 4096;
constexpr int k_poll_interval_ms = 250;
constexpr unsigned k_max_read_failures = 8;

std::string cursor(std::string_view nonce, char lane, std::uint64_t index)
{
    return "v1:" + std::string(nonce) + ":" + lane + ":" + std::to_string(index);
}

std::optional<std::uint64_t> cursor_index(std::string_view value, std::string_view nonce, char lane)
{
    if (value.empty()) return 0;
    const auto prefix = "v1:" + std::string(nonce) + ":" + lane + ":";
    if (!value.starts_with(prefix)) return std::nullopt;
    value.remove_prefix(prefix.size());
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return std::nullopt;
    std::uint64_t result = 0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), result);
    if (converted.ec != std::errc() || converted.ptr != value.data() + value.size()) return std::nullopt;
    return result;
}

std::string label(std::string_view name)
{
    // Native assigned names are UTF-8. Continuous lifecycle labels allow 768
    // bytes and 256 UTF-16 units; truncate only at a complete code point.
    std::size_t bytes = 0;
    std::size_t units = 0;
    while (bytes < name.size()) {
        const auto first = static_cast<unsigned char>(name[bytes]);
        const std::size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
        const std::size_t next_units = units + (width == 4 ? 2 : 1);
        if (bytes + width > name.size() || bytes + width > 768 || next_units > 256) break;
        bytes += width;
        units = next_units;
    }
    return std::string(name.substr(0, bytes));
}

struct Child_event {
    addon::Lifecycle_observation observation;
    addon::Child_status status;
};

addon::Child_status terminal_status(Native_child_state state)
{
    switch (state) {
        case Native_child_state::idle:        return addon::Child_status::SUCCEEDED;
        case Native_child_state::turn_failed: return addon::Child_status::FAILED;
        case Native_child_state::interrupted: return addon::Child_status::INTERRUPTED;
        default:                              return addon::Child_status::UNKNOWN;
    }
}

Child_event child_observation(std::string_view owner, const Native_child& child,
    addon::Child_status status, std::string_view state_name)
{
    addon::Lifecycle_observation result;
    result.provider_event_name = "vibe_subagent_" + std::string(state_name);
    result.provider_session_id = owner;
    // Vibe keeps one child session across successive runs. Use the native turn
    // identity for its public execution span, so a stopped span cannot be
    // resurrected by a later message to the same child.
    result.agent_id = child.turn_id;
    result.agent_label = label(child.name);
    result.subagent_transition = status == addon::Child_status::RUNNING ?
        addon::Subagent_transition::STARTED : addon::Subagent_transition::STOPPED;
    // This is an observed child activity fact, not an outcome of the root turn.
    // The host's root phase/outcome fields deliberately stay neutral.
    std::string identity;
    for (const auto value : {owner, std::string_view(child.session_id), std::string_view(child.spawn_action_id),
            std::string_view(child.turn_id), state_name}) {
        identity.append(value);
        identity.push_back('\0');
    }
    result.provider_event_id = "vibe-child:" + addon::to_hex(addon::Sha256::digest(identity));
    return {std::move(result), status};
}

std::vector<Child_event> differences(std::string_view owner,
    const std::map<std::string, Native_child>& previous, const std::vector<Native_child>& current, bool initial)
{
    using Status = addon::Child_status;
    std::vector<Child_event> result;
    for (const auto& child : current) {
        const auto found = previous.find(child.session_id);
        const auto* old = found == previous.end() ? nullptr : &found->second;
        const bool was_active = old && native_child_active(old->state);
        const bool active = native_child_active(child.state);
        const auto old_generation = old ? old->turn_generation.value_or(0) : 0;
        const bool later_run = child.turn_generation && *child.turn_generation > old_generation;
        if (active && !was_active) {
            result.push_back(child_observation(owner, child, Status::RUNNING, native_child_state_name(child.state)));
        }
        else if (!active && was_active) {
            if (later_run) {
                result.push_back(child_observation(owner, *old, Status::UNKNOWN, "turn_advanced"));
                result.push_back(child_observation(owner, child, Status::RUNNING, "recorded_run"));
            }
            // Tombstones/deletion records no longer retain a turn. Their stop
            // belongs to the exact execution this observer previously started.
            const auto& stopped = child.turn_id.empty() ? *old : child;
            result.push_back(child_observation(owner, stopped, terminal_status(child.state), native_child_state_name(child.state)));
        }
        else if (!initial && !active && later_run) {
            // The hook-gated baseline distinguishes earlier history from a
            // new run that finished between samples. Its committed native
            // outcome proves the run existed even when running was not seen.
            result.push_back(child_observation(owner, child, Status::RUNNING, "recorded_run"));
            result.push_back(child_observation(owner, child, terminal_status(child.state), native_child_state_name(child.state)));
        }
        else if (active && was_active && later_run) {
            // A later native generation proves the previous activity ended,
            // even when its intervening idle state fell between two samples.
            // It does not prove a successful/failed/interrupted outcome.
            result.push_back(child_observation(owner, *old, Status::UNKNOWN, "turn_advanced"));
            result.push_back(child_observation(owner, child, Status::RUNNING, native_child_state_name(child.state)));
        }
    }
    for (const auto& [id, child] : previous) {
        if (native_child_active(child.state) && std::none_of(current.begin(), current.end(), [&](const auto& item) {
                return item.session_id == id;
            })) {
            result.push_back(child_observation(owner, child, Status::UNKNOWN, "removed"));
        }
    }
    return result;
}

} // namespace

struct Observer::Impl {
    struct Queued {
        std::uint64_t index = 0;
        Child_event event;
    };
    struct Binding {
        std::string session_id;
        std::filesystem::path transcript;
        std::string current_sha256;
        std::string generation;
        std::map<std::string, Native_child> children;
        std::deque<Queued> queue;
        std::optional<addon::Child_loss> child_loss;
        std::uint64_t produced = 0;
        std::uint64_t acknowledged = 0;
        std::uint64_t delivered = 0;
        bool observing = false;
        bool retired = false;
    };

    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<Binding>, std::less<>> bindings;

    std::shared_ptr<Binding> find(std::string_view nonce) const
    {
        const auto item = bindings.find(nonce);
        return item == bindings.end() ? nullptr : item->second;
    }

    std::string accept_snapshot(Binding& binding, Native_snapshot snapshot, std::uint64_t& added)
    {
        if (!binding.generation.empty() && snapshot.generation < binding.generation) return "vibe_store_generation_regressed";
        for (const auto& child : snapshot.children) {
            const auto old = binding.children.find(child.session_id);
            if (old != binding.children.end() && old->second.turn_generation && child.turn_generation &&
                *child.turn_generation < *old->second.turn_generation) return "vibe_child_generation_regressed";
        }
        auto batch = differences(binding.session_id, binding.children, snapshot.children, binding.current_sha256.empty());
        if (binding.queue.size() + batch.size() > k_max_queued_observations) {
            return "vibe_observer_queue_full";
        }
        for (auto& child : batch) {
            const auto index = ++binding.produced;
            child.observation.provider_sequence = std::to_string(index);
            binding.queue.push_back({index, std::move(child)});
            ++added;
        }
        binding.children.clear();
        for (auto& child : snapshot.children) binding.children.emplace(child.session_id, std::move(child));
        binding.current_sha256 = std::move(snapshot.current_sha256);
        binding.generation = std::move(snapshot.generation);
        return {};
    }

    void observe(addon::Invocation_context& context)
    {
        addon::Contract_failure failure;
        const auto request = addon::decode_observe_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (!request->correlation) {
            context.fail(addon::Result_status::UNAVAILABLE, "vibe_correlation_required");
            return;
        }
        const auto nonce = request->correlation->transition_nonce;
        std::shared_ptr<Binding> binding;
        std::string error;
        {
            std::lock_guard lock(mutex);
            binding = find(nonce);
            const auto index = cursor_index(request->resume_cursor, nonce, 'w');
            if (!binding || binding->session_id.empty()) error = "vibe_correlation_unbound";
            else if (binding->session_id != request->session.provider_session_id) error = "vibe_observer_session_mismatch";
            else if (!index || *index > binding->produced) error = "vibe_observer_cursor_invalid";
            else if (binding->observing) error = "vibe_observer_already_running";
            else binding->observing = true;
        }
        if (!error.empty()) {
            context.fail(addon::Result_status::UNAVAILABLE, error);
            return;
        }
        struct Watch_guard {
            Impl& state;
            std::shared_ptr<Binding> binding;
            ~Watch_guard() { std::lock_guard lock(state.mutex); binding->observing = false; }
        } guard{*this, binding};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(request->window_ms);
        const auto wait_for_sample = [&] {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining > 0) {
                context.wait_for_cancel(static_cast<int>(std::min<std::int64_t>(k_poll_interval_ms, remaining)));
            }
        };
        std::uint64_t available = 0;
        {
            std::lock_guard lock(mutex);
            available = binding->produced - binding->acknowledged;
        }
        if (available) context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
        unsigned failures = 0;
        while (!context.cancel_requested() && std::chrono::steady_clock::now() < deadline) {
            std::string known;
            bool coverage_lost;
            {
                std::lock_guard lock(mutex);
                if (binding->retired) break;
                known = binding->current_sha256;
                coverage_lost = binding->child_loss.has_value();
            }
            if (coverage_lost) {
                // A fresh native activation establishes the next child baseline.
                // Continue checking for retirement while the watch is held.
                wait_for_sample();
                continue;
            }
            auto read = read_native_snapshot(binding->transcript, binding->session_id, known);
            std::uint64_t added = 0;
            if (read.snapshot) {
                if (read.snapshot->identity.kind == Native_session_kind::subagent) {
                    context.fail(addon::Result_status::UNAVAILABLE, "vibe_observer_requires_main_session");
                    return;
                }
                std::lock_guard lock(mutex);
                if (binding->retired) break;
                if (std::count_if(read.snapshot->children.begin(), read.snapshot->children.end(), [](const auto& child) {
                        return native_child_active(child.state);
                    }) > 128) {
                    error = "vibe_subagent_capacity";
                }
                else {
                    error = accept_snapshot(*binding, std::move(*read.snapshot), added);
                }
                failures = 0;
            }
            else if (read.unchanged) failures = 0;
            else if (++failures >= k_max_read_failures) {
                // A broken/unfamiliar store is unavailable evidence, never an
                // empty graph or a fabricated successful stop of its workers.
                if (read.reason == "vibe_store_child_invalid" || read.reason == "vibe_store_children_invalid") {
                    error = read.reason;
                }
                else {
                    context.fail(addon::Result_status::UNAVAILABLE, read.reason);
                    return;
                }
            }
            if (!error.empty()) {
                if (request->child_state_version != 1 || error == "vibe_store_generation_regressed") {
                    const auto status = (error == "vibe_subagent_capacity" || !read.reason.empty()) ?
                        addon::Result_status::UNAVAILABLE : addon::Result_status::FAILED;
                    context.fail(status, error);
                    return;
                }
                std::lock_guard lock(mutex);
                if (binding->retired) break;
                binding->child_loss = addon::Child_loss{
                    binding->session_id, nonce, ++binding->produced, error, "vibe.subagents"};
                ++added;
            }
            if (added) {
                available += added;
                context.report_progress(available, std::nullopt, addon::observe_progress_note(available));
            }
            wait_for_sample();
        }
        addon::Observe_result result;
        result.model_failure = request->model_failure;
        result.child_failure = request->child_failure;
        result.execution_failure = request->execution_failure;
        if (request->context_failure) {
            result.context_failure = addon::Context_failure{binding->session_id, *request->context_failure};
        }
        else if (request->context_state_version == 2) {
            auto native = read_native_context(binding->transcript, binding->session_id);
            if (native.update) result.context_updates.push_back(std::move(*native.update));
            result.context_failure = std::move(native.failure);
        }
        {
            std::lock_guard lock(mutex);
            result.resume_cursor = cursor(nonce, 'w', binding->produced);
        }
        // The SDK protocol completes cancelled watch windows successfully; a
        // watch result never acknowledges a polled page or discards its queue.
        context.succeed(addon::encode_observe_result(result));
    }

    void poll(addon::Invocation_context& context)
    {
        addon::Contract_failure failure;
        const auto request = addon::decode_poll_observations_request(context.request().payload, &failure);
        if (!request) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        if (!request->correlation) {
            context.fail(addon::Result_status::UNAVAILABLE, "vibe_correlation_required");
            return;
        }
        addon::Poll_observations_result result;
        result.model_failure = request->model_failure;
        result.child_failure = request->child_failure;
        result.execution_failure = request->execution_failure;
        std::string error;
        const auto nonce = request->correlation->transition_nonce;
        std::shared_ptr<Binding> binding;
        {
            std::lock_guard lock(mutex);
            binding = find(nonce);
            const auto index = cursor_index(request->resume_cursor, nonce, 'p');
            if (!binding || binding->session_id.empty()) error = "vibe_correlation_unbound";
            else if (!index || *index < binding->acknowledged || *index > binding->delivered) error = "vibe_observer_cursor_invalid";
            else {
                // The incoming cursor acknowledges the last page the host
                // committed. The page being returned stays available to retry.
                binding->acknowledged = *index;
                while (!binding->queue.empty() && binding->queue.front().index <= *index) binding->queue.pop_front();
                auto last = *index;
                std::size_t count = 0;
                for (const auto& queued : binding->queue) {
                    if (count++ == request->limit) break;
                    const auto& observation = queued.event.observation;
                    result.observations.push_back(observation);
                    if (request->child_state_version == 1) {
                        addon::Child_update update;
                        update.provider_session_id = binding->session_id;
                        update.transition_nonce    = nonce;
                        update.sequence            = queued.index;
                        update.agent_id            = observation.agent_id;
                        update.depth               = 1;
                        update.status              = queued.event.status;
                        update.source              = observation.provider_event_name;
                        result.child_updates.push_back(std::move(update));
                    }
                    last = queued.index;
                }
                if (result.observations.empty() && binding->child_loss && last < binding->child_loss->sequence) {
                    if (request->child_state_version == 1) {
                        result.child_loss = binding->child_loss;
                        last = binding->child_loss->sequence;
                    }
                    else if (!request->child_failure) {
                        error = binding->child_loss->reason_code;
                    }
                }
                binding->delivered = std::max(binding->delivered, last);
                result.resume_cursor = cursor(nonce, 'p', last);
                result.more = last < binding->produced && (!result.observations.empty() || !result.child_failure);
            }
        }
        if (!error.empty()) {
            context.fail(addon::Result_status::UNAVAILABLE, error);
            return;
        }
        if (request->context_failure) {
            result.context_failure = addon::Context_failure{binding->session_id, *request->context_failure};
        }
        else if (request->context_state_version == 2) {
            auto native = read_native_context(binding->transcript, binding->session_id);
            if (native.update) result.context_updates.push_back(std::move(*native.update));
            result.context_failure = std::move(native.failure);
        }
        context.succeed(addon::encode_poll_observations_result(result));
    }
};

Observer::Observer() : m_impl(std::make_unique<Impl>()) {}
Observer::~Observer() = default;

Native_context_result Observer::read_context(std::string_view transition_nonce, std::string_view provider_session_id,
    std::optional<std::uint64_t> count_revision) const
{
    std::filesystem::path transcript;
    {
        std::lock_guard lock(m_impl->mutex);
        const auto binding = m_impl->find(transition_nonce);
        if (!binding || binding->session_id != provider_session_id) {
            return {std::nullopt, addon::Context_failure{std::string(provider_session_id),
                addon::Contract_failure{"vibe_context_binding_unavailable", {}}}};
        }
        transcript = binding->transcript;
    }
    return read_native_context(transcript, provider_session_id, count_revision);
}

bool Observer::reserved(std::string_view transition_nonce) const
{
    std::lock_guard lock(m_impl->mutex);
    return static_cast<bool>(m_impl->find(transition_nonce));
}

bool Observer::bind(std::string_view transition_nonce, std::string_view provider_session_id,
    const std::filesystem::path& transcript)
{
    if (provider_session_id.empty() || !transcript.is_absolute() || transcript.lexically_normal() != transcript ||
        addon::path_to_utf8(transcript.filename()) != provider_session_id ||
        transcript.parent_path().filename() != "unified") return false;
    std::shared_ptr<Impl::Binding> binding;
    {
        std::lock_guard lock(m_impl->mutex);
        binding = m_impl->find(transition_nonce);
        if (!binding) return false;
        if (!binding->session_id.empty()) return binding->session_id == provider_session_id && binding->transcript == transcript;
    }
    // session_start gates the first user message, so a successful read here
    // establishes a baseline before that message can spawn fast workers.
    // Missing optional store evidence must not suppress native root identity.
    auto baseline = read_native_snapshot(transcript, provider_session_id);
    {
        std::lock_guard lock(m_impl->mutex);
        if (binding->retired || m_impl->find(transition_nonce) != binding) return false;
        if (!binding->session_id.empty()) return binding->session_id == provider_session_id && binding->transcript == transcript;
        binding->session_id = provider_session_id;
        binding->transcript = transcript;
        if (baseline.snapshot && baseline.snapshot->identity.kind != Native_session_kind::subagent &&
            std::count_if(baseline.snapshot->children.begin(), baseline.snapshot->children.end(), [](const auto& child) {
                return native_child_active(child.state);
            }) <= 128) {
            std::uint64_t ignored = 0;
            m_impl->accept_snapshot(*binding, std::move(*baseline.snapshot), ignored);
        }
    }
    return true;
}

void Observer::handle_lifetime(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto& request = context.request();
    if (request.operation == addon::k_lifecycle_correlation_lifetime_reserve_operation) {
        const auto decoded = addon::decode_lifecycle_correlation_reserve_request(request.payload, &failure);
        if (!decoded) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        bool full = false;
        {
            std::lock_guard lock(m_impl->mutex);
            if (!m_impl->find(decoded->correlation.transition_nonce)) {
                full = m_impl->bindings.size() >= k_max_correlations;
                if (!full) m_impl->bindings.emplace(decoded->correlation.transition_nonce, std::make_shared<Impl::Binding>());
            }
        }
        if (full) context.fail(addon::Result_status::UNAVAILABLE, "vibe_correlation_capacity");
        else context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
    }
    else if (request.operation == addon::k_lifecycle_correlation_lifetime_retire_operation) {
        const auto decoded = addon::decode_lifecycle_correlation_retire_request(request.payload, &failure);
        if (!decoded) {
            context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
            return;
        }
        bool invalid_terminal = false;
        {
            std::lock_guard lock(m_impl->mutex);
            const auto binding = m_impl->find(decoded->correlation.transition_nonce);
            if (binding) {
                // Root END comes from the native helper lane, so this observer
                // does not produce a terminal page. Its host retirement is
                // abandoned; a terminal-consumed claim cannot discard a queue.
                invalid_terminal = decoded->disposition == addon::Lifecycle_correlation_retire_disposition::TERMINAL_CONSUMED;
                if (!invalid_terminal) {
                    binding->retired = true;
                    binding->children.clear();
                    binding->queue.clear();
                    m_impl->bindings.erase(decoded->correlation.transition_nonce);
                }
            }
        }
        if (invalid_terminal) context.fail(addon::Result_status::FAILED, "vibe_observer_terminal_cursor_invalid");
        else context.succeed(addon::encode_lifecycle_correlation_lifetime_ack());
    }
    else context.reject_unknown_operation();
}

void Observer::handle_observation(addon::Invocation_context& context)
{
    if (context.request().operation == addon::k_observe_operation) m_impl->observe(context);
    else if (context.request().operation == addon::k_poll_observations_operation) m_impl->poll(context);
    else context.reject_unknown_operation();
}

} // namespace vibe
