#include "vibe_context.h"
#include "vibe_projection_fixture.h"

#include <iostream>

namespace {
using namespace vibe_test;

void native_measurements()
{
    Store_fixture store;
    auto state = runtime();
    state.set("core_settings", json(R"({"context":{"model_facts":{"context_window":2048}}})"));
    store.publish(state);
    auto value = projection();
    publish_projection(store, value);
    auto result = vibe::read_native_context(store.transcript, "root-session", 41);
    require(result.update && !result.failure && result.update->used_tokens == 90 &&
        result.update->context_window == 2048 && result.update->compaction_count == 0 &&
        result.update->compaction_count_revision == 41 && !result.update->compaction_count_complete,
        "native context confused session billing, capacity, or history authority");
    result = vibe::read_native_context(store.transcript, "root-session");
    require(result.update && !result.update->compaction_count && !result.update->compaction_count_revision,
        "continuous reader invented absolute-history sampling authority");

    value.find("snapshot")->find("session")->set("contextUsage", addon::Json_value());
    publish_projection(store, value);
    result = vibe::read_native_context(store.transcript, "root-session");
    require(result.update && result.update->usage_unavailable && !result.update->used_tokens &&
        !result.update->context_window, "missing native context became zero or retained stale capacity");
    value.find("snapshot")->find("session")->set("contextUsage",
        json(R"({"inputTokens":10,"outputTokens":2,"totalTokens":13})"));
    publish_projection(store, value);
    result = vibe::read_native_context(store.transcript, "root-session");
    require(result.failure && result.failure->provider_session_id == "root-session" && !result.update,
        "invalid source context was not independently scoped");
}

void compaction_evidence()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    value.find("snapshot")->find("session")->set("contextUsage",
        json(R"({"inputTokens":0,"outputTokens":0,"totalTokens":0})"));
    const auto success = json(R"({"type":"checkpoint","kind":"compaction","id":"checkpoint-compaction-1","sessionId":"root-session","generationStatus":"completed","details":{"summaryLength":50}})");
    const auto failure = json(R"({"type":"checkpoint","kind":"compaction","id":"checkpoint-compaction-2","sessionId":"root-session","generationStatus":"completed","details":{"error":{"code":"invalid_compaction_summary"}}})");
    auto entries = addon::Json_value::array();
    entries.push_back(success);
    entries.push_back(failure);
    entries.push_back(success);
    publish_projection(store, value, {entries});
    auto result = vibe::read_native_context(store.transcript, "root-session", 42);
    require(result.update && result.update->compaction_count == 1 && result.update->usage_unavailable &&
        !result.update->used_tokens && !result.update->compacted,
        "failed/duplicate compaction inflated counts or unmeasured summary became empty context");
    value.find("snapshot")->find("session")->set("contextUsage",
        json(R"({"inputTokens":20,"outputTokens":3,"totalTokens":23})"));
    publish_projection(store, value, {entries});
    write(store.transcript / "journal/0000000000000002.jsonl", "pending native advance\n");
    result = vibe::read_native_context(store.transcript, "root-session", 43);
    require(result.update && result.update->usage_unavailable && result.update->compaction_count == 1,
        "lagging committed snapshot claimed live occupancy or discarded proved compaction lower bound");
}
} // namespace

int main()
{
    try {
        native_measurements();
        compaction_evidence();
        std::cout << "Vibe native context tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
