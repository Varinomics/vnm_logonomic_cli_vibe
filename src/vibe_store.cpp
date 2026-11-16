#include "vibe_store.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>
#include <utility>

namespace vibe {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

namespace {

// The native writer's document limit is 64 MiB. The envelopes can contain
// unrelated retained actions; exceeding the limit never means an empty graph.
constexpr std::size_t k_document_bytes = 64u * 1024u * 1024u;
constexpr std::size_t k_pointer_bytes = 16u * 1024u;
constexpr std::size_t k_manifest_bytes = 1024u * 1024u;
constexpr std::size_t k_child_limit = 256;

const std::string* text(const addon::Json_value& object, std::string_view key)
{
    const auto* value = object.find(key);
    return value && value->is_string() ? &value->as_string() : nullptr;
}

std::optional<std::uint64_t> number(const addon::Json_value& object, std::string_view key)
{
    const auto* value = object.find(key);
    if (!value || !value->is_integer() || value->as_integer() < 0) return std::nullopt;
    return static_cast<std::uint64_t>(value->as_integer());
}

bool identifier(std::string_view value)
{
    return !value.empty() && value.size() <= 256 && value.front() != '-' && value.front() != '_' &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        });
}

bool bounded_text(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum && value.find('\0') == std::string_view::npos;
}

bool filename(std::string_view value)
{
    return bounded_text(value, 256) && value != "." && value != ".." &&
        value.find_first_of("/\\:") == std::string_view::npos;
}

bool regular_directory(const fs::path& path)
{
    std::error_code error;
    return fs::is_directory(fs::symlink_status(path, error)) && !error;
}

std::optional<std::string> document(const fs::path& path, std::size_t maximum)
{
    std::error_code error;
    if (!fs::is_regular_file(fs::symlink_status(path, error)) || error) return std::nullopt;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > maximum) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string result(static_cast<std::size_t>(size), '\0');
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input || input.peek() != std::char_traits<char>::eof() || result.back() != '\n') return std::nullopt;
    result.pop_back();
    return result;
}

std::optional<addon::Json_value> parse(std::string_view bytes, std::size_t maximum)
{
    addon::Json_parse_limits limits;
    limits.max_bytes = maximum;
    limits.max_string_bytes = maximum;
    limits.max_members = 65536;
    limits.max_depth = 64;
    return addon::parse_json(bytes, limits, nullptr);
}

bool same(const addon::Json_value& value, std::string_view key, std::string_view expected)
{
    const auto* actual = text(value, key);
    return actual && *actual == expected;
}

bool optional_identity(const addon::Json_value& object, std::string_view key,
    std::optional<std::string>& result)
{
    const auto* value = object.find(key);
    if (!value || value->is_null()) return true;
    if (!value->is_string() || !identifier(value->as_string())) return false;
    result = value->as_string();
    return true;
}

std::optional<Native_identity> identity(const addon::Json_value& runtime, std::string_view session_id)
{
    const auto* encoded = runtime.find("identity");
    const auto* metadata = runtime.find("session_metadata");
    if (!encoded || !encoded->is_object() || !metadata || !metadata->is_object()) return std::nullopt;
    const auto* kind = text(*encoded, "kind");
    const auto* root = text(*encoded, "root_session_id");
    const auto depth = number(*encoded, "depth");
    Native_identity result;
    result.session_id = session_id;
    if (!kind || !root || !identifier(*root) || !depth ||
        !same(*encoded, "session_id", session_id) ||
        !optional_identity(*encoded, "parent_session_id", result.parent_session_id)) return std::nullopt;
    result.root_session_id = *root;
    if (*kind == "root" && *depth == 0 && !result.parent_session_id && *root == session_id) {
        result.kind = Native_session_kind::root;
    }
    else if (*kind == "fork" && *depth == 0 && result.parent_session_id && *result.parent_session_id != session_id) {
        result.kind = Native_session_kind::fork;
    }
    else if (*kind == "subagent" && *depth == 1 && result.parent_session_id &&
        *result.parent_session_id != session_id && *root != session_id) {
        result.kind = Native_session_kind::subagent;
    }
    else return std::nullopt;
    std::optional<std::string> metadata_parent;
    if (!same(*metadata, "root_session_id", *root) ||
        !optional_identity(*metadata, "parent_session_id", metadata_parent) || metadata_parent != result.parent_session_id) return std::nullopt;
    const auto* spawn = metadata->find("subagent_spawn_key");
    if (result.kind == Native_session_kind::subagent) {
        if (!spawn || !spawn->is_string() || !bounded_text(spawn->as_string(), 256)) return std::nullopt;
    }
    else if (spawn && !spawn->is_null()) return std::nullopt;
    return result;
}

bool child_turn(const addon::Json_value& encoded, Native_child& child)
{
    const auto generation = number(encoded, "generation");
    const auto* turn = text(encoded, "turn_id");
    if (!generation || *generation == 0 || !turn ||
        *turn != child.session_id + ":turn:" + std::to_string(*generation) || turn->size() > 256) return false;
    child.turn_generation = *generation;
    child.turn_id = *turn;
    return true;
}

std::optional<Native_child> child(const addon::Json_member& member, const Native_identity& owner)
{
    const auto& encoded = member.value;
    const auto* id = text(encoded, "child_session_id");
    const auto* name = text(encoded, "agent_name");
    const auto* spawn = text(encoded, "spawn_action_id");
    const auto* state = encoded.find("state");
    const auto* type = state ? text(*state, "type") : nullptr;
    const auto depth = number(encoded, "depth");
    if (!id || !identifier(*id) || *id == owner.session_id || *id == owner.root_session_id ||
        !name || *name != member.name || !bounded_text(*name, 4096) ||
        !spawn || !bounded_text(*spawn, 256) || !depth || *depth != 1 || !type) return std::nullopt;
    Native_child result;
    result.session_id = *id;
    result.parent_session_id = owner.session_id;
    result.root_session_id = owner.root_session_id;
    result.name = *name;
    result.spawn_action_id = *spawn;
    const auto* profile = encoded.find("agent_type");
    if (profile && !profile->is_null()) {
        if (!profile->is_string() || !bounded_text(profile->as_string(), 4096)) return std::nullopt;
        result.agent_type = profile->as_string();
    }
    if (*type == "reserved") result.state = Native_child_state::reserved;
    else if (*type == "running" || *type == "deleting_running") {
        result.state = *type == "running" ? Native_child_state::running : Native_child_state::deleting_running;
        const auto* active = state->find("active");
        if (!active || !child_turn(*active, result)) return std::nullopt;
    }
    else if (*type == "idle") {
        const auto* outcome = state->find("last_outcome");
        const auto* outcome_type = outcome ? text(*outcome, "type") : nullptr;
        if (!outcome_type || (*outcome_type != "completed" && *outcome_type != "interrupted") ||
            !child_turn(*outcome, result)) return std::nullopt;
        result.state = *outcome_type == "completed" ? Native_child_state::idle : Native_child_state::interrupted;
    }
    else if (*type == "turn_failed") {
        result.state = Native_child_state::turn_failed;
        const auto* outcome = state->find("outcome");
        if (!outcome || !same(*outcome, "type", "failed") || !child_turn(*outcome, result)) return std::nullopt;
    }
    else if (*type == "creation_failed") result.state = Native_child_state::creation_failed;
    else if (*type == "creation_cleanup_pending") result.state = Native_child_state::creation_cleanup_pending;
    else if (*type == "deleting_idle") result.state = Native_child_state::deleting_idle;
    else if (*type == "tombstone") result.state = Native_child_state::tombstone;
    else return std::nullopt;
    return result;
}

Native_store_result failure(std::string reason)
{
    return {std::nullopt, false, std::move(reason)};
}

} // namespace

Native_store_result read_native_snapshot(const fs::path& transcript, std::string_view expected_session_id,
    std::string_view previous_current_sha256)
{
    if (!identifier(expected_session_id) || !transcript.is_absolute() ||
        transcript.lexically_normal() != transcript || transcript.filename() != addon::path_from_utf8(expected_session_id) ||
        transcript.parent_path().filename() != "unified") return failure("vibe_store_identity_invalid");
    if (!regular_directory(transcript.parent_path()) || !regular_directory(transcript) ||
        !regular_directory(transcript / "generations")) return failure("vibe_store_path_unavailable");
    const auto current_bytes = document(transcript / "CURRENT", k_pointer_bytes);
    if (!current_bytes) return failure("vibe_store_current_unavailable");
    const auto current_digest = addon::to_hex(addon::Sha256::digest(*current_bytes));
    if (!previous_current_sha256.empty() && current_digest == previous_current_sha256) return {std::nullopt, true, {}};
    const auto current = parse(*current_bytes, k_pointer_bytes);
    const auto* generation = current ? text(*current, "generation") : nullptr;
    const auto* manifest_digest = current ? text(*current, "manifest_sha256") : nullptr;
    const auto sequence = current ? number(*current, "snapshot_sequence") : std::nullopt;
    if (!current || !same(*current, "store_format", "mistral.vibe.unified-session-store/v1") ||
        !same(*current, "session_id", expected_session_id) || !generation || generation->size() != 16 ||
        !std::all_of(generation->begin(), generation->end(), [](char ch) { return ch >= '0' && ch <= '9'; }) ||
        !manifest_digest || !addon::sha256_from_hex(*manifest_digest) || !sequence) return failure("vibe_store_current_invalid");
    if (const auto* minor = current->find("store_format_minor"); minor &&
        (!minor->is_integer() || minor->as_integer() < 1 || minor->as_integer() > 10)) return failure("vibe_store_version_unsupported");
    const auto generation_directory = transcript / "generations" / *generation;
    if (!regular_directory(generation_directory)) return failure("vibe_store_generation_unavailable");
    const auto manifest_bytes = document(generation_directory / "manifest.json", k_manifest_bytes);
    if (!manifest_bytes || addon::to_hex(addon::Sha256::digest(*manifest_bytes)) != *manifest_digest) return failure("vibe_store_manifest_invalid");
    const auto manifest = parse(*manifest_bytes, k_manifest_bytes);
    if (!manifest || number(*manifest, "manifest_version") != 1 ||
        !same(*manifest, "session_id", expected_session_id) || !same(*manifest, "generation", *generation) ||
        number(*manifest, "snapshot_sequence") != sequence) return failure("vibe_store_manifest_invalid");
    const auto* descriptor = manifest->find("runtime_state");
    const auto* runtime_path = descriptor ? text(*descriptor, "path") : nullptr;
    const auto* runtime_digest = descriptor ? text(*descriptor, "sha256") : nullptr;
    if (!runtime_path || !filename(*runtime_path) || !runtime_digest || !addon::sha256_from_hex(*runtime_digest)) return failure("vibe_store_runtime_descriptor_invalid");
    const auto runtime_bytes = document(generation_directory / addon::path_from_utf8(*runtime_path), k_document_bytes);
    if (!runtime_bytes || addon::to_hex(addon::Sha256::digest(*runtime_bytes)) != *runtime_digest) return failure("vibe_store_runtime_invalid");
    const auto runtime = parse(*runtime_bytes, k_document_bytes);
    if (!runtime || number(*runtime, "runtime_state_version") != 3 ||
        !same(*runtime, "session_id", expected_session_id) || number(*runtime, "snapshot_sequence") != sequence) return failure("vibe_store_runtime_invalid");
    auto native_identity = identity(*runtime, expected_session_id);
    if (!native_identity) return failure("vibe_store_identity_invalid");
    Native_snapshot result;
    result.identity = std::move(*native_identity);
    result.generation = *generation;
    result.snapshot_sequence = *sequence;
    result.current_sha256 = current_digest;
    const auto* metadata = runtime->find("session_metadata");
    const auto* model = metadata ? metadata->find("active_model") : nullptr;
    // An empty pin means "follow default". The effective alias is written at
    // the next native turn; optional model evidence never invalidates children.
    if (model && model->is_string() && bounded_text(model->as_string(), 256)) {
        result.selected_model_alias = model->as_string();
    }
    if (const auto* cwd = metadata ? text(*metadata, "cwd") : nullptr;
        cwd && bounded_text(*cwd, 32768) && addon::path_from_utf8(*cwd).is_absolute())
    {
        result.working_directory = *cwd;
    }
    const auto* settings = runtime->find("core_settings");
    const auto* context = settings ? settings->find("context") : nullptr;
    const auto* facts = context ? context->find("model_facts") : nullptr;
    const auto window = facts ? number(*facts, "context_window") : std::nullopt;
    if (window && *window > 0 && *window <= 9007199254740991ULL) {
        result.context_window = window;
    }
    const auto* subagents = runtime->find("subagents");
    if (subagents && !subagents->is_null()) {
        if (result.identity.kind == Native_session_kind::subagent) return failure("vibe_store_nested_children_unsupported");
        const auto* children = subagents->find("children");
        if (!children || !children->is_object() || children->as_object().size() > k_child_limit) return failure("vibe_store_children_invalid");
        std::set<std::string> ids;
        for (const auto& item : children->as_object()) {
            auto parsed = child(item, result.identity);
            if (!parsed || !ids.insert(parsed->session_id).second) return failure("vibe_store_child_invalid");
            result.children.push_back(std::move(*parsed));
        }
        std::sort(result.children.begin(), result.children.end(), [](const auto& a, const auto& b) {
            return a.session_id < b.session_id;
        });
    }
    // Publication is an atomic CURRENT replacement. Do not mix a read with a
    // newer generation, or mistake generation cleanup for a lost child.
    const auto after = document(transcript / "CURRENT", k_pointer_bytes);
    if (!after || *after != *current_bytes) return failure("vibe_store_generation_changed");
    return {std::move(result), false, {}};
}

Native_projection_result read_native_projection(
    const fs::path& transcript, const Native_snapshot& snapshot, bool include_history)
{
    const auto unavailable = [](std::string reason) {
        return Native_projection_result{std::nullopt, false, std::move(reason)};
    };
    if (!identifier(snapshot.identity.session_id) || !transcript.is_absolute() ||
        transcript.lexically_normal() != transcript ||
        transcript.filename() != addon::path_from_utf8(snapshot.identity.session_id) ||
        transcript.parent_path().filename() != "unified")
    {
        return unavailable("vibe_store_identity_invalid");
    }
    const auto current_bytes = document(transcript / "CURRENT", k_pointer_bytes);
    if (!current_bytes || addon::to_hex(addon::Sha256::digest(*current_bytes)) != snapshot.current_sha256) {
        return unavailable("vibe_store_generation_changed");
    }
    const auto current = parse(*current_bytes, k_pointer_bytes);
    const auto* manifest_digest = current ? text(*current, "manifest_sha256") : nullptr;
    if (!current || !manifest_digest || !same(*current, "generation", snapshot.generation) ||
        !same(*current, "session_id", snapshot.identity.session_id))
    {
        return unavailable("vibe_store_current_invalid");
    }
    const auto directory = transcript / "generations" / snapshot.generation;
    if (!regular_directory(directory)) {
        return unavailable("vibe_store_generation_unavailable");
    }
    const auto manifest_bytes = document(directory / "manifest.json", k_manifest_bytes);
    if (!manifest_bytes || addon::to_hex(addon::Sha256::digest(*manifest_bytes)) != *manifest_digest) {
        return unavailable("vibe_store_manifest_invalid");
    }
    const auto manifest = parse(*manifest_bytes, k_manifest_bytes);
    if (!manifest || !same(*manifest, "session_id", snapshot.identity.session_id) ||
        !same(*manifest, "generation", snapshot.generation) ||
        number(*manifest, "snapshot_sequence") != snapshot.snapshot_sequence)
    {
        return unavailable("vibe_store_manifest_invalid");
    }
    const auto* descriptor = manifest->find("projection_state");
    const auto* path = descriptor ? text(*descriptor, "path") : nullptr;
    const auto* digest = descriptor ? text(*descriptor, "sha256") : nullptr;
    if (!path || !filename(*path) || !digest || !addon::sha256_from_hex(*digest)) {
        return unavailable("vibe_store_projection_descriptor_invalid");
    }
    const auto bytes = document(directory / addon::path_from_utf8(*path), k_document_bytes);
    if (!bytes || addon::to_hex(addon::Sha256::digest(*bytes)) != *digest) {
        return unavailable("vibe_store_projection_invalid");
    }
    auto projection = parse(*bytes, k_document_bytes);
    if (!projection || number(*projection, "projection_state_version") != 1 ||
        !same(*projection, "session_id", snapshot.identity.session_id) ||
        number(*projection, "snapshot_sequence") != snapshot.snapshot_sequence)
    {
        return unavailable("vibe_store_projection_invalid");
    }
    auto* state = projection->find("snapshot");
    const auto* session = state ? state->find("session") : nullptr;
    if (!state || !same(*state, "format", "harness.public-session-state/v1") ||
        !session || !same(*session, "id", snapshot.identity.session_id))
    {
        return unavailable("vibe_store_projection_identity_invalid");
    }
    if (include_history) {
        auto* history = state->find("history");
        auto* entries = history ? history->find("entries") : nullptr;
        if (!entries || !entries->is_array()) {
            return unavailable("vibe_store_projection_history_invalid");
        }
        const auto* chunks = descriptor->find("chunks");
        if (chunks && !chunks->is_null()) {
            if (!chunks->is_array() || !entries->as_array().empty() ||
                !regular_directory(transcript / "chunks"))
            {
                return unavailable("vibe_store_projection_chunks_invalid");
            }
            std::size_t total_bytes = bytes->size();
            for (const auto& chunk : chunks->as_array()) {
                if (!chunk.is_string() || !addon::sha256_from_hex(chunk.as_string())) {
                    return unavailable("vibe_store_projection_chunks_invalid");
                }
                const auto chunk_bytes = document(
                    transcript / "chunks" / (chunk.as_string() + ".json"), k_document_bytes);
                if (!chunk_bytes || total_bytes > k_document_bytes - chunk_bytes->size() ||
                    addon::to_hex(addon::Sha256::digest(*chunk_bytes)) != chunk.as_string())
                {
                    return unavailable("vibe_store_projection_chunk_invalid");
                }
                total_bytes += chunk_bytes->size();
                auto parsed = parse(*chunk_bytes, k_document_bytes);
                if (!parsed || !parsed->is_array()) {
                    return unavailable("vibe_store_projection_chunk_invalid");
                }
                for (auto& entry : parsed->as_array()) {
                    entries->push_back(std::move(entry));
                }
            }
        }
    }
    const auto* journal = manifest->find("recovery_journal_segment");
    const auto* journal_path = journal ? text(*journal, "path") : nullptr;
    const auto first = journal ? number(*journal, "first_sequence") : std::nullopt;
    if (!journal_path || !first || *first != snapshot.snapshot_sequence + 1 ||
        !journal_path->starts_with("journal/") || !filename(std::string_view(*journal_path).substr(8)))
    {
        return unavailable("vibe_store_journal_descriptor_invalid");
    }
    std::error_code error;
    const auto journal_file = transcript / addon::path_from_utf8(*journal_path);
    if (!regular_directory(transcript / "journal") ||
        !fs::is_regular_file(fs::symlink_status(journal_file, error)) || error)
    {
        return unavailable("vibe_store_journal_unavailable");
    }
    const auto journal_bytes = fs::file_size(journal_file, error);
    if (error) {
        return unavailable("vibe_store_journal_unavailable");
    }
    const auto after = document(transcript / "CURRENT", k_pointer_bytes);
    if (!after || *after != *current_bytes) {
        return unavailable("vibe_store_generation_changed");
    }
    return {std::move(projection), journal_bytes != 0, {}};
}

bool native_child_active(Native_child_state state)
{
    return state == Native_child_state::reserved || state == Native_child_state::running;
}

std::string_view native_child_state_name(Native_child_state state)
{
    switch (state) {
    case Native_child_state::reserved: return "reserved";
    case Native_child_state::running: return "running";
    case Native_child_state::idle: return "idle";
    case Native_child_state::interrupted: return "interrupted";
    case Native_child_state::creation_failed: return "creation_failed";
    case Native_child_state::creation_cleanup_pending: return "creation_cleanup_pending";
    case Native_child_state::turn_failed: return "turn_failed";
    case Native_child_state::deleting_running: return "deleting_running";
    case Native_child_state::deleting_idle: return "deleting_idle";
    case Native_child_state::tombstone: return "tombstone";
    }
    return "unknown";
}

} // namespace vibe
