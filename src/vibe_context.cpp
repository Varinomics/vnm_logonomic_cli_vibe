#include "vibe_context.h"

#include <limits>
#include <set>

namespace vibe {
namespace {
namespace addon = logonomic::cli_addon;

constexpr std::uint64_t k_max_exact_integer = 9007199254740991;

std::optional<std::uint64_t> integer(const addon::Json_value* value)
{
    if (!value || !value->is_integer() || value->as_integer() < 0 ||
        static_cast<std::uint64_t>(value->as_integer()) > k_max_exact_integer)
    {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value->as_integer());
}

bool equals(const addon::Json_value* value, std::string_view text)
{
    return value && value->is_string() && value->as_string() == text;
}

Native_context_result failed(std::string_view session_id, std::string reason)
{
    return {std::nullopt, addon::Context_failure{
        std::string(session_id), addon::Contract_failure{std::move(reason), {}}}};
}

} // namespace

Native_context_result read_native_context(
    const std::filesystem::path& transcript, std::string_view expected_session_id,
    std::optional<std::uint64_t> count_revision)
{
    const auto store = read_native_snapshot(transcript, expected_session_id);
    if (!store.snapshot) {
        return failed(expected_session_id, store.reason);
    }
    const auto projection = read_native_projection(transcript, *store.snapshot, count_revision.has_value());
    if (!projection.projection) {
        return failed(store.snapshot->identity.session_id, projection.reason);
    }
    const auto* snapshot = projection.projection->find("snapshot");
    const auto* session = snapshot->find("session");
    addon::Context_update update;
    update.provider_session_id = store.snapshot->identity.session_id;
    update.event_id = "vibe-context:" + store.snapshot->current_sha256;
    if (count_revision) {
        if (*count_revision == 0 || *count_revision > k_max_exact_integer) {
            return failed(update.provider_session_id, "vibe_context_revision_invalid");
        }
        std::set<std::string> successful;
        const auto* entries = snapshot->find("history")->find("entries");
        for (const auto& entry : entries->as_array()) {
            if (!equals(entry.find("type"), "checkpoint") || !equals(entry.find("kind"), "compaction") ||
                !equals(entry.find("generationStatus"), "completed"))
            {
                continue;
            }
            const auto* details = entry.find("details");
            if (!details || !details->is_object() || details->find("error")) {
                continue;
            }
            const auto* id = entry.find("id");
            if (!equals(entry.find("sessionId"), update.provider_session_id) || !id ||
                !id->is_string() || id->as_string().empty() || !integer(details->find("summaryLength")))
            {
                return failed(update.provider_session_id, "vibe_context_history_invalid");
            }
            successful.insert(id->as_string());
        }
        update.compaction_count = successful.size();
        update.compaction_count_revision = count_revision;
        // CURRENT authenticates the available history, not the absence of an
        // earlier repair/import. Never make an incomplete resume look like zero.
        update.compaction_count_complete = false;
    }
    const auto* usage = session->find("contextUsage");
    if (projection.journal_pending || !usage || usage->is_null()) {
        update.usage_unavailable = true;
    }
    else {
        const auto input = integer(usage->find("inputTokens"));
        const auto output = integer(usage->find("outputTokens"));
        const auto total = integer(usage->find("totalTokens"));
        if (!input || !output || !total || *input > k_max_exact_integer - *output || *input + *output != *total) {
            return failed(update.provider_session_id, "vibe_context_usage_invalid");
        }
        // Vibe writes all-zero context after compaction: the replacement
        // summary is unmeasured until the next completion. It is not empty.
        if (*total == 0) {
            update.usage_unavailable = true;
        }
        else {
            update.used_tokens = total;
            update.context_window = store.snapshot->context_window;
        }
    }
    return {std::move(update), std::nullopt};
}

} // namespace vibe
