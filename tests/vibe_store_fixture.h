#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/sha256.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vibe_test {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

inline void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

inline addon::Json_value json(std::string_view input)
{
    auto parsed = addon::parse_json(input, {}, nullptr);
    require(parsed.has_value(), "invalid test JSON");
    return std::move(*parsed);
}

inline void write(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "could not write test store");
}

inline std::string canonical(const addon::Json_value& value)
{
    return addon::serialize_json_canonical(value);
}

inline addon::Json_value runtime(std::string_view session = "root-session")
{
    auto result = json(R"({"runtime_state_version":3,"snapshot_sequence":1,"session_id":"root-session","identity":{"kind":"root","depth":0,"session_id":"root-session","root_session_id":"root-session","parent_session_id":null},"session_metadata":{"root_session_id":"root-session","parent_session_id":null,"subagent_spawn_key":null,"active_model":"selected-model"},"subagents":{"children":{}}})");
    result.set("session_id", addon::Json_value(session));
    result.find("identity")->set("session_id", addon::Json_value(session));
    result.find("identity")->set("root_session_id", addon::Json_value(session));
    result.find("session_metadata")->set("root_session_id", addon::Json_value(session));
    return result;
}

inline addon::Json_value child(std::string_view state = "running", std::uint64_t generation = 1,
    std::string_view id = "child-one", std::string_view name = "explorer-1")
{
    auto result = json(R"({"child_session_id":"child-one","agent_name":"explorer-1","agent_type":null,"spawn_action_id":"spawn-action","depth":1,"state":{}})");
    result.set("child_session_id", addon::Json_value(id));
    result.set("agent_name", addon::Json_value(name));
    auto status = addon::Json_value::object();
    status.set("type", addon::Json_value(state));
    auto turn = addon::Json_value::object();
    turn.set("generation", addon::Json_value(generation));
    turn.set("turn_id", addon::Json_value(std::string(id) + ":turn:" + std::to_string(generation)));
    if (state == "running" || state == "deleting_running") status.set("active", turn);
    if (state == "idle" || state == "interrupted") {
        status.set("type", addon::Json_value("idle"));
        turn.set("type", addon::Json_value(state == "idle" ? "completed" : "interrupted"));
        status.set("last_outcome", turn);
    }
    if (state == "turn_failed") {
        turn.set("type", addon::Json_value("failed"));
        status.set("outcome", turn);
    }
    result.set("state", std::move(status));
    return result;
}

inline void set_child(addon::Json_value& value, const addon::Json_value& entry)
{
    value.find("subagents")->find("children")->set(entry.find("agent_name")->as_string(), entry);
}

class Store_fixture {
public:
    fs::path root;
    fs::path transcript;
    fs::path generation;
    addon::Json_value current;
    addon::Json_value manifest;
    std::uint64_t counter = 0;

    explicit Store_fixture(std::string_view id = "root-session")
    {
        static std::uint64_t unique = 0;
        root = fs::temp_directory_path() / ("vibe-store-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++unique));
        transcript = root / "unified" / std::string(id);
        fs::create_directories(transcript / "generations");
    }

    ~Store_fixture()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    void publish(const addon::Json_value& state)
    {
        const auto generation_number = std::to_string(++counter);
        const auto name = std::string(16 - generation_number.size(), '0') + generation_number;
        generation = transcript / "generations" / name;
        fs::create_directories(generation);
        const auto bytes = canonical(state);
        write(generation / "runtime-state.json", bytes + "\n");
        manifest = addon::Json_value::object();
        manifest.set("manifest_version", addon::Json_value(1));
        manifest.set("session_id", *state.find("session_id"));
        manifest.set("generation", addon::Json_value(name));
        manifest.set("snapshot_sequence", *state.find("snapshot_sequence"));
        auto descriptor = addon::Json_value::object();
        descriptor.set("path", addon::Json_value("runtime-state.json"));
        descriptor.set("sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(bytes))));
        manifest.set("runtime_state", std::move(descriptor));
        current = addon::Json_value::object();
        current.set("store_format", addon::Json_value("mistral.vibe.unified-session-store/v1"));
        current.set("store_format_minor", addon::Json_value(10));
        current.set("session_id", *state.find("session_id"));
        current.set("generation", addon::Json_value(name));
        current.set("snapshot_sequence", *state.find("snapshot_sequence"));
        publish_manifest();
    }

    void publish_manifest()
    {
        const auto bytes = canonical(manifest);
        write(generation / "manifest.json", bytes + "\n");
        current.set("manifest_sha256", addon::Json_value(addon::to_hex(addon::Sha256::digest(bytes))));
        write(transcript / "CURRENT", canonical(current) + "\n");
    }
};

} // namespace vibe_test
