#include "vibe_store.h"
#include "vibe_projection_fixture.h"

#include <iostream>

namespace {
namespace fixture = vibe_test;
namespace addon = logonomic::cli_addon;

void projection_generation()
{
    fixture::Store_fixture store;
    auto runtime = fixture::runtime();
    runtime.find("session_metadata")->set("active_model", addon::Json_value(""));
    runtime.find("session_metadata")->set("cwd", addon::Json_value(store.root.generic_string()));
    runtime.set("core_settings", fixture::json(
        R"({"context":{"model_facts":{"context_window":262144}}})"));
    store.publish(runtime);
    fixture::publish_projection(store, fixture::projection(), {
        fixture::json(R"([{"id":"first","type":"message"}])"),
        fixture::json(R"([{"id":"last","type":"checkpoint"}])")});
    auto snapshot = vibe::read_native_snapshot(store.transcript, "root-session");
    fixture::require(snapshot.snapshot && !snapshot.snapshot->selected_model_alias,
        "following the native default suppressed the session");
    fixture::require(snapshot.snapshot->context_window == 262144 &&
        snapshot.snapshot->working_directory == store.root.generic_string(),
        "native context capacity or workspace was lost");
    auto loaded = vibe::read_native_projection(store.transcript, *snapshot.snapshot);
    fixture::require(loaded.projection && !loaded.journal_pending, "valid generation was rejected");
    const auto& entries = loaded.projection->find("snapshot")->find("history")->find("entries")->as_array();
    fixture::require(entries.size() == 2 && entries[0].find("id")->as_string() == "first" &&
        entries[1].find("id")->as_string() == "last", "native chunk order was not preserved");
    fixture::write(store.transcript / "journal/0000000000000002.jsonl", "pending native event\n");
    loaded = vibe::read_native_projection(store.transcript, *snapshot.snapshot, false);
    fixture::require(loaded.projection && loaded.journal_pending,
        "unfolded journal was presented as current live state");
    store.publish(runtime);
    fixture::require(!vibe::read_native_projection(store.transcript, *snapshot.snapshot).projection,
        "a different generation was mixed with the requested snapshot");
}

void projection_integrity()
{
    fixture::Store_fixture store;
    store.publish(fixture::runtime());
    auto value = fixture::projection();
    value.find("snapshot")->find("session")->set("id", addon::Json_value("foreign-session"));
    fixture::publish_projection(store, value);
    auto snapshot = vibe::read_native_snapshot(store.transcript, "root-session");
    fixture::require(snapshot.snapshot.has_value(), "projection failure suppressed child graph");
    fixture::require(!vibe::read_native_projection(store.transcript, *snapshot.snapshot).projection,
        "foreign projection identity was accepted");
    fixture::publish_projection(store, fixture::projection(), {
        fixture::json(R"([{"id":"one"}])")});
    snapshot = vibe::read_native_snapshot(store.transcript, "root-session");
    const auto digest = store.manifest.find("projection_state")->find("chunks")->as_array()[0].as_string();
    fixture::write(store.transcript / "chunks" / (digest + ".json"), "[]\n");
    fixture::require(!vibe::read_native_projection(store.transcript, *snapshot.snapshot).projection,
        "corrupt history chunk was accepted");
    fixture::require(vibe::read_native_projection(store.transcript, *snapshot.snapshot, false).projection.has_value(),
        "unrequested history corruption suppressed independent usage evidence");
}
} // namespace

int main()
{
    try {
        projection_generation();
        projection_integrity();
        std::cout << "Vibe native projection tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
