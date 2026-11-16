#pragma once

#include "vibe_store_fixture.h"

#include <vector>

namespace vibe_test {

inline addon::Json_value projection(std::string_view session_id = "root-session")
{
    auto value = json(R"({"projection_state_version":1,"session_id":"root-session","snapshot_sequence":1,"snapshot":{"format":"harness.public-session-state/v1","session":{"id":"root-session","createdAt":1794777703388,"updatedAt":1794777704122,"tokenUsage":{"inputTokens":120,"outputTokens":30,"totalTokens":150,"cachedInputTokens":0},"contextUsage":{"inputTokens":80,"outputTokens":10,"totalTokens":90,"cachedInputTokens":0}},"history":{"range":"latest","cursor":{"before":null,"after":null},"entries":[]}}})");
    value.set("session_id", addon::Json_value(session_id));
    value.find("snapshot")->find("session")->set("id", addon::Json_value(session_id));
    return value;
}

inline void publish_projection(
    Store_fixture& store, const addon::Json_value& value,
    const std::vector<addon::Json_value>& chunks = {})
{
    const auto bytes = canonical(value);
    write(store.generation / "projection-state.json", bytes + "\n");
    auto descriptor = addon::Json_value::object();
    descriptor.set("path", addon::Json_value("projection-state.json"));
    descriptor.set("sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(bytes))));
    if (!chunks.empty()) {
        fs::create_directories(store.transcript / "chunks");
        auto references = addon::Json_value::array();
        for (const auto& chunk : chunks) {
            const auto chunk_bytes = canonical(chunk);
            const auto digest = addon::to_hex(addon::Sha256::digest(chunk_bytes));
            write(store.transcript / "chunks" / (digest + ".json"), chunk_bytes + "\n");
            references.push_back(addon::Json_value(digest));
        }
        descriptor.set("chunks", std::move(references));
    }
    store.manifest.set("projection_state", std::move(descriptor));
    store.manifest.set("recovery_journal_segment",
        json(R"({"first_sequence":2,"path":"journal/0000000000000002.jsonl"})"));
    fs::create_directories(store.transcript / "journal");
    write(store.transcript / "journal/0000000000000002.jsonl", "");
    store.publish_manifest();
}

} // namespace vibe_test
