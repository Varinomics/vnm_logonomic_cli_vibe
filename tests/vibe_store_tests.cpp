#include "vibe_store.h"
#include "vibe_store_fixture.h"

#include <logonomic_cli_addon/package_source.h>

#include <array>
#include <iostream>

namespace {
using namespace vibe_test;

void identities()
{
    Store_fixture store;
    auto state = runtime();
    store.publish(state);
    auto read = vibe::read_native_snapshot(store.transcript, "root-session");
    require(read.snapshot.has_value(), "valid native root snapshot rejected");
    require(read.snapshot->children.empty(), "empty root invented a child");
    const auto again = vibe::read_native_snapshot(store.transcript, "root-session", read.snapshot->current_sha256);
    require(again.unchanged && !again.snapshot && again.reason.empty(), "unchanged pointer did not use bounded cache");

    state.find("identity")->set("kind", addon::Json_value("fork"));
    state.find("identity")->set("root_session_id", addon::Json_value("ancestor"));
    state.find("identity")->set("parent_session_id", addon::Json_value("fork-source"));
    state.find("session_metadata")->set("root_session_id", addon::Json_value("ancestor"));
    state.find("session_metadata")->set("parent_session_id", addon::Json_value("fork-source"));
    set_child(state, child());
    store.publish(state);
    read = vibe::read_native_snapshot(store.transcript, "root-session");
    require(read.snapshot && read.snapshot->identity.kind == vibe::Native_session_kind::fork,
        "main fork was mistaken for a child");
    require(read.snapshot->children.front().parent_session_id == "root-session" &&
        read.snapshot->children.front().root_session_id == "ancestor", "fork child ancestry was flattened incorrectly");

    state.find("identity")->set("kind", addon::Json_value("subagent"));
    state.find("identity")->set("depth", addon::Json_value(1));
    state.find("session_metadata")->set("subagent_spawn_key", addon::Json_value("spawn-key"));
    state.set("subagents", addon::Json_value());
    store.publish(state);
    read = vibe::read_native_snapshot(store.transcript, "root-session");
    require(read.snapshot && read.snapshot->identity.kind == vibe::Native_session_kind::subagent,
        "native child session identity was rejected");
    state.set("subagents", json(R"({"children":{}})"));
    store.publish(state);
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot,
        "unsupported nested child controller was accepted");
}

void child_states()
{
    constexpr std::array names = {"reserved", "running", "idle", "interrupted", "creation_failed",
        "creation_cleanup_pending", "turn_failed", "deleting_running", "deleting_idle", "tombstone"};
    Store_fixture store;
    for (const auto* name : names) {
        auto state = runtime();
        auto entry = child(name);
        entry.set("agent_name", addon::Json_value("探索者-1"));
        entry.set("agent_type", addon::Json_value("researcher"));
        set_child(state, entry);
        store.publish(state);
        const auto read = vibe::read_native_snapshot(store.transcript, "root-session");
        require(read.snapshot && read.snapshot->children.size() == 1, "native child state rejected");
        const auto& actual = read.snapshot->children.front();
        require(vibe::native_child_state_name(actual.state) == name, "native child outcome changed meaning");
        require(actual.name == "探索者-1" && actual.agent_type == "researcher", "assigned name conflated with profile");
        require(vibe::native_child_active(actual.state) == (std::string_view(name) == "running" ||
            std::string_view(name) == "reserved"), "nonrunning child remained active");
    }
    auto state = runtime();
    set_child(state, child("idle"));
    state.find("subagents")->find("children")->find("explorer-1")->find("state")->find("last_outcome")
        ->set("turn_id", addon::Json_value("another-child:turn:1"));
    store.publish(state);
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "foreign child turn was accepted");
    state = runtime();
    set_child(state, child("future-state"));
    store.publish(state);
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "unknown child state became idle");
    state = runtime();
    set_child(state, child());
    set_child(state, child("running", 1, "child-one", "duplicate"));
    store.publish(state);
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "duplicate child identity accepted");
}

void corruption()
{
    Store_fixture store;
    store.publish(runtime());
    require(!vibe::read_native_snapshot(store.transcript, "other-session").snapshot, "cross-session path accepted");
    write(store.generation / "runtime-state.json", "{}\n");
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "runtime digest mismatch accepted");
    store.publish(runtime());
    write(store.generation / "manifest.json", "{}\n");
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "manifest digest mismatch accepted");
    store.publish(runtime());
    store.manifest.find("runtime_state")->set("path", addon::Json_value("../runtime-state.json"));
    store.publish_manifest();
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "runtime traversal accepted");
    store.publish(runtime());
    store.current.set("store_format_minor", addon::Json_value(11));
    write(store.transcript / "CURRENT", canonical(store.current) + "\n");
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "future native format accepted");
    store.publish(runtime());
    write(store.transcript / "CURRENT", canonical(store.current));
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "partial current publication accepted");
    auto state = runtime();
    state.find("session_metadata")->set("root_session_id", addon::Json_value("another-root"));
    store.publish(state);
    require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "contradictory metadata identity accepted");
    store.publish(runtime());
    const auto target = store.root / "outside.json";
    fs::rename(store.generation / "runtime-state.json", target);
    std::error_code error;
    fs::create_symlink(target, store.generation / "runtime-state.json", error);
    if (!error) require(!vibe::read_native_snapshot(store.transcript, "root-session").snapshot, "symlinked runtime accepted");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--inspect") {
            const auto path = addon::path_from_utf8(argv[2]);
            const auto result = vibe::read_native_snapshot(path, addon::path_to_utf8(path.filename()));
            require(result.snapshot.has_value(), result.reason.c_str());
            std::cout << "generation=" << result.snapshot->generation << " children=" << result.snapshot->children.size() << '\n';
            for (const auto& item : result.snapshot->children) {
                std::cout << item.session_id << ' ' << item.name << ' ' << vibe::native_child_state_name(item.state)
                    << ' ' << item.turn_id << '\n';
            }
            return 0;
        }
        identities();
        child_states();
        corruption();
        std::cout << "Vibe native store tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
