#include "vibe_usage.h"

#include "vibe_launcher.h"
#include "vibe_store.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>
#include <logonomic_cli_addon/usage_session.h>

#include <toml++/toml.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace vibe {
namespace {
namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

constexpr std::string_view k_cache_format = "vibe-native-usage-v1";
constexpr std::uint64_t k_max_exact_integer = 9007199254740991;
constexpr std::size_t k_cache_line_bytes = 256 * 1024;

class Usage_error final : public std::runtime_error
{
public:
    explicit Usage_error(std::string_view reason) :
        std::runtime_error(reason.empty() ? "vibe_usage_contract_invalid" : std::string(reason)) {}
};

void require(bool condition, std::string_view reason)
{
    if (!condition) {
        throw Usage_error(reason);
    }
}

void check_cancel(addon::Invocation_context& context)
{
    require(!context.cancel_requested(), "cancelled");
}

std::string digest(std::string_view value)
{
    return addon::to_hex(addon::Sha256::digest(value));
}

const std::string& text(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    require(member && member->is_string(), "vibe_usage_evidence_invalid");
    return member->as_string();
}

std::uint64_t integer(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    require(member && member->is_integer() && member->as_integer() >= 0 &&
        static_cast<std::uint64_t>(member->as_integer()) <= k_max_exact_integer,
        "vibe_usage_evidence_invalid");
    return static_cast<std::uint64_t>(member->as_integer());
}

addon::Json_value parse(std::string_view bytes)
{
    const auto parsed = addon::parse_json(bytes, {k_cache_line_bytes, 32, 256, k_cache_line_bytes}, nullptr);
    require(parsed.has_value(), "vibe_usage_cache_invalid");
    return *parsed;
}

std::string read_file(const fs::path& path, std::size_t maximum)
{
    std::error_code error;
    require(fs::is_regular_file(fs::symlink_status(path, error)) && !error,
        "vibe_usage_file_unavailable");
    const auto size = fs::file_size(path, error);
    require(!error && size <= maximum, "vibe_usage_file_invalid");
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(input.good() && input.peek() == std::char_traits<char>::eof(), "vibe_usage_file_changed");
    return bytes;
}

void write_file(const fs::path& path, std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "vibe_usage_cache_write_failed");
}

std::string record_id(std::string_view session_id)
{
    return "vibe-session:" + digest(session_id);
}

struct Saved_record
{
    addon::Usage_record usage;
    std::string generation;
    std::uint64_t sequence = 0;
    std::string current_sha256;
};

addon::Json_value encode_saved(const Saved_record& record)
{
    auto result = addon::Json_value::object();
    result.set("record", addon::encode_usage_record(record.usage));
    result.set("generation", addon::Json_value(record.generation));
    result.set("sequence", addon::Json_value(record.sequence));
    result.set("current_sha256", addon::Json_value(record.current_sha256));
    return result;
}

Saved_record decode_saved(const addon::Json_value& value)
{
    addon::Contract_failure failure;
    const auto* member = value.find("record");
    const auto record = member ? addon::decode_usage_record(*member, &failure) : std::nullopt;
    require(record && record->session_key && record->record_id == record_id(*record->session_key) &&
        record->semantics == addon::Usage_counter_semantics::CUMULATIVE_FINAL && !record->model,
        "vibe_usage_cache_invalid");
    Saved_record result{*record, text(value, "generation"), integer(value, "sequence"), text(value, "current_sha256")};
    require(result.generation.size() == 16 && result.generation.find_first_not_of("0123456789") == std::string::npos &&
        addon::sha256_from_hex(result.current_sha256).has_value(), "vibe_usage_cache_invalid");
    return result;
}

using Records = std::map<std::string, Saved_record>;

Records load_records(const fs::path& root, addon::Invocation_context& context)
{
    std::ifstream input(root / "records.jsonl", std::ios::binary);
    require(input.good(), "vibe_usage_cache_unavailable");
    Records records;
    std::string previous;
    std::string line;
    while (std::getline(input, line)) {
        check_cancel(context);
        require(!line.empty() && line.size() <= k_cache_line_bytes, "vibe_usage_cache_invalid");
        auto record = decode_saved(parse(line));
        require(previous.empty() || previous < record.usage.record_id, "vibe_usage_cache_invalid");
        previous = record.usage.record_id;
        records.emplace(previous, std::move(record));
        context.report_progress(records.size(), std::nullopt, "Reading saved usage");
    }
    require(input.eof() && !input.bad(), "vibe_usage_cache_unavailable");
    return records;
}

addon::Usage_cache_manifest validate_cache(const addon::Usage_cache_reference& reference,
    const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    auto manifest = addon::validate_usage_cache_candidate(reference, request.context.profile_storage->root,
        request.context.account_context, [&] { return context.cancel_requested(); }, &failure);
    require(manifest.has_value(), failure.reason_code);
    require(manifest->format == k_cache_format && manifest->files.size() == 2 &&
        manifest->files[0].path == "metadata.json" && manifest->files[1].path == "records.jsonl",
        "vibe_usage_cache_format_unsupported");
    return *manifest;
}

fs::path expanded_root(std::string value, const addon::Environment& environment, std::string_view cwd)
{
    if (value == "~" || value.starts_with("~/") || value.starts_with("~\\")) {
#ifdef _WIN32
        const auto home = addon::environment_value(environment, "USERPROFILE");
#else
        const auto home = addon::environment_value(environment, "HOME");
#endif
        require(!home.empty(), "vibe_usage_home_unavailable");
        value = home + value.substr(1);
    }
    auto path = addon::path_from_utf8(value);
    if (path.is_relative()) {
        path = addon::path_from_utf8(cwd) / path;
    }
    std::error_code error;
    path = fs::weakly_canonical(path, error);
    require(!error && path.is_absolute(), "vibe_usage_root_unavailable");
    return path;
}

std::set<fs::path> source_roots(const addon::Usage_scan_request& request)
{
    const auto environment = addon::environment_pairs(request.context.launch_environment->effective_environment);
    const auto home = resolve_home(environment, request.context.working_directory);
    require(home.has_value(), "vibe_usage_home_unavailable");
    auto cwd = request.context.working_directory;
    if (const auto selected_cwd = addon::environment_value(environment, "VIBE_CWD"); !selected_cwd.empty()) {
        cwd = addon::path_to_utf8(expanded_root(selected_cwd, environment, cwd));
    }
    // These are discovered history locations, not a reimplementation of Vibe's
    // live layered configuration. Other project/agent/admin roots remain an
    // explicitly incomplete historical scope.
    std::set<fs::path> roots{*home / "logs" / "session"};
    const auto selected = addon::environment_value(environment, "VIBE_SESSION_LOGGING__SAVE_DIR");
    if (!selected.empty()) {
        roots.insert(expanded_root(selected, environment, cwd));
    }
    const auto config = *home / "config.toml";
    std::error_code error;
    const auto config_status = fs::symlink_status(config, error);
    require(!error || error == std::errc::no_such_file_or_directory, "vibe_usage_config_unavailable");
    if (fs::exists(config_status)) {
        try {
            const auto parsed = toml::parse(read_file(config, 4 * 1024 * 1024));
            if (const auto path = parsed["session_logging"]["save_dir"].value<std::string>(); path && !path->empty()) {
                roots.insert(expanded_root(*path, environment, cwd));
            }
        }
        catch (const toml::parse_error&) {
            throw Usage_error("vibe_usage_config_invalid");
        }
    }
    return roots;
}

addon::Usage_amount known(std::uint64_t value)
{
    return {true, value, addon::Usage_field_coverage{1, 1, false, addon::Usage_support::SUPPORTED}};
}

addon::Usage_amount unknown(std::optional<std::uint64_t> value = std::nullopt)
{
    return {true, value, addon::Usage_field_coverage{0, 1, false, addon::Usage_support::UNKNOWN}};
}

std::string date(std::uint64_t milliseconds)
{
    // The compatibility field names the native snapshot's UTC date. Temporal
    // evidence below explicitly leaves lifetime tokens unallocated by date.
    require(milliseconds <= 253402300799999, "vibe_usage_timestamp_invalid");
    const auto day = std::chrono::floor<std::chrono::days>(std::chrono::sys_time<std::chrono::milliseconds>{
        std::chrono::milliseconds(milliseconds)});
    const std::chrono::year_month_day calendar(day);
    const int year = static_cast<int>(calendar.year());
    require(calendar.ok() && year > 0 && year <= 9999, "vibe_usage_timestamp_invalid");
    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) <<
        static_cast<unsigned>(calendar.month()) << '-' << std::setw(2) << static_cast<unsigned>(calendar.day());
    return output.str();
}

struct Native_usage_read
{
    std::string current_sha256;
    std::optional<Saved_record> record;
};

Native_usage_read read_record(const fs::path& transcript)
{
    const auto session_id = addon::path_to_utf8(transcript.filename());
    const auto native = read_native_snapshot(transcript, session_id);
    require(native.snapshot.has_value(), native.reason);
    const auto projection = read_native_projection(transcript, *native.snapshot, false);
    require(projection.projection.has_value(), projection.reason);
    const auto* session = projection.projection->find("snapshot")->find("session");
    const auto* counts = session->find("tokenUsage");
    if (!counts || counts->is_null()) {
        return {native.snapshot->current_sha256, std::nullopt};
    }
    const auto input = integer(*counts, "inputTokens");
    const auto output = integer(*counts, "outputTokens");
    const auto total = integer(*counts, "totalTokens");
    std::optional<std::uint64_t> cached;
    const auto* cache_member = counts->find("cachedInputTokens");
    if (cache_member && !cache_member->is_null()) {
        cached = integer(*counts, "cachedInputTokens");
    }
    require(input <= k_max_exact_integer - output && input + output == total && (!cached || *cached <= input),
        "vibe_usage_counters_invalid");
    addon::Usage_record usage;
    usage.record_id = record_id(session_id);
    usage.session_key = session_id;
    usage.completed_date = date(integer(*session, "updatedAt"));
    usage.total_tokens = total;
    usage.source_path = addon::path_to_utf8(transcript);
    usage.input = known(input);
    usage.output = known(output);
    // The projection normalizes absent native cache evidence to zero and drops
    // cache-write counters. Positive reads are a lower bound, never complete
    // cache coverage; zero is unknown rather than a claimed measured zero.
    usage.cache_read = unknown(cached && *cached ? cached : std::nullopt);
    usage.cache_creation = unknown();
    usage.coverage_unit = addon::Usage_coverage_unit::SESSIONS;
    usage.counter_field = "session.tokenUsage.totalTokens";
    usage.source_counter_semantics = "cumulative_final";
    usage.aggregate_evidence = "partial";
    // The native active_model alias is a current pin, not attribution of
    // accumulated billing. Native child calls do not persist per-call models.
    addon::Contract_failure failure;
    require(addon::decode_usage_record(addon::encode_usage_record(usage), &failure).has_value(),
        "vibe_usage_record_invalid");
    return {native.snapshot->current_sha256, Saved_record{std::move(usage), native.snapshot->generation,
        native.snapshot->snapshot_sequence, native.snapshot->current_sha256}};
}

void merge_record(Records& records, Saved_record incoming)
{
    const auto existing = records.find(incoming.usage.record_id);
    if (existing != records.end()) {
        const auto& before = existing->second;
        require(incoming.generation >= before.generation && incoming.sequence >= before.sequence,
            "vibe_usage_source_regressed");
        require(incoming.generation != before.generation || incoming.current_sha256 == before.current_sha256,
            "vibe_usage_generation_conflict");
        require(incoming.usage.total_tokens >= before.usage.total_tokens &&
            incoming.usage.input.value >= before.usage.input.value &&
            incoming.usage.output.value >= before.usage.output.value &&
            (!before.usage.cache_read.value || (incoming.usage.cache_read.value &&
                *incoming.usage.cache_read.value >= *before.usage.cache_read.value)),
            "vibe_usage_counter_regressed");
    }
    const auto key = incoming.usage.record_id;
    records.insert_or_assign(key, std::move(incoming));
}

void scan_sources(Records& records, const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    std::map<std::string, std::string> seen;
    std::uint64_t inspected = 0;
    for (const auto& root : source_roots(request)) {
        const auto unified = root / "unified";
        std::error_code error;
        const auto status = fs::symlink_status(unified, error);
        if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) {
            continue;
        }
        require(!error && fs::is_directory(status), "vibe_usage_root_unavailable");
        fs::directory_iterator entry(unified, error);
        require(!error, "vibe_usage_root_unavailable");
        const fs::directory_iterator end;
        for (; entry != end; entry.increment(error)) {
            require(!error, "vibe_usage_root_unavailable");
            check_cancel(context);
            const auto child_status = entry->symlink_status(error);
            require(!error, "vibe_usage_source_unavailable");
            if (!fs::is_directory(child_status)) {
                continue;
            }
            auto native = read_record(entry->path());
            const auto key = record_id(addon::path_to_utf8(entry->path().filename()));
            // A verified native copy is an identity witness even when it has
            // no counters. Reject conflicting copies independently of order.
            const auto [prior, inserted] = seen.emplace(key, native.current_sha256);
            require(inserted || prior->second == native.current_sha256, "vibe_usage_duplicate_source_conflict");
            if (native.record) {
                if (inserted) merge_record(records, std::move(*native.record));
            }
            else {
                require(!records.contains(key), "vibe_usage_counter_unavailable");
            }
            context.report_progress(++inspected, std::nullopt, "Reading native Vibe usage");
        }
        require(!error, "vibe_usage_root_unavailable");
    }
}

std::string request_digest(addon::Usage_scan_request request)
{
    request.snapshot_id.reset();
    request.cursor.reset();
    return digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(request)));
}

struct Snapshot
{
    addon::Usage_cache_reference reference;
    addon::Observation_scope scope;
    std::string request_sha256;
    Records records;
};

Snapshot load_snapshot(const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const bool root_valid = addon::validate_usage_cache_root(request.cache_selection->candidate_root,
        request.context.profile_storage->root, &failure);
    require(root_valid, failure.reason_code);
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root));
    const auto manifest_bytes = read_file(root / "cache-manifest.json", k_cache_line_bytes);
    const auto manifest = addon::decode_usage_cache_manifest(parse(manifest_bytes), &failure);
    require(manifest.has_value(), "vibe_usage_cache_invalid");
    Snapshot snapshot;
    snapshot.reference = {request.cache_selection->candidate_root, manifest->snapshot_id, digest(manifest_bytes)};
    validate_cache(snapshot.reference, request, context);
    const auto metadata = parse(read_file(root / "metadata.json", k_cache_line_bytes));
    const auto* scope = metadata.find("scope");
    const auto observation = scope ? addon::decode_observation_scope(*scope, &failure) : std::nullopt;
    require(observation && !observation->source_account_key, "vibe_usage_cache_invalid");
    snapshot.scope = *observation;
    snapshot.request_sha256 = text(metadata, "request_sha256");
    require(snapshot.request_sha256 == request_digest(request), "vibe_usage_snapshot_context_changed");
    require(!request.snapshot_id || *request.snapshot_id == snapshot.reference.snapshot_id,
        "vibe_usage_snapshot_changed");
    snapshot.records = load_records(root, context);
    return snapshot;
}

Snapshot create_snapshot(const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const bool allocation_valid = addon::validate_usage_cache_allocation(request.cache_selection->candidate_root,
        request.context.profile_storage->root, &failure);
    require(allocation_valid, failure.reason_code);
    Snapshot snapshot;
    snapshot.request_sha256 = request_digest(request);
    snapshot.scope.observed_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    snapshot.scope.fresh_for_ms = 60000;
    const auto id = "vibe-usage:" + digest(snapshot.request_sha256 + ":" + std::to_string(snapshot.scope.observed_at_ms));
    if (request.cache_selection->accepted) {
        validate_cache(*request.cache_selection->accepted, request, context);
        snapshot.records = load_records(addon::usage_cache_io_path(
            addon::path_from_utf8(request.cache_selection->accepted->root)), context);
    }
    scan_sources(snapshot.records, request, context);
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(request.cache_selection->candidate_root));
    std::ofstream records_file(root / "records.jsonl", std::ios::binary | std::ios::trunc);
    require(records_file.good(), "vibe_usage_cache_write_failed");
    for (auto& [key, saved] : snapshot.records) {
        check_cancel(context);
        addon::Usage_temporal temporal;
        temporal.parent = {key, saved.usage.total_tokens, id, std::nullopt};
        temporal.unavailable_tokens = saved.usage.total_tokens;
        temporal.coverage_reason = "unsupported";
        saved.usage.temporal = std::move(temporal);
        const auto line = addon::serialize_json_canonical(encode_saved(saved));
        require(line.size() <= k_cache_line_bytes, "vibe_usage_record_too_large");
        records_file << line << '\n';
    }
    records_file.close();
    require(records_file.good(), "vibe_usage_cache_write_failed");
    auto metadata = addon::Json_value::object();
    metadata.set("request_sha256", addon::Json_value(snapshot.request_sha256));
    metadata.set("scope", addon::encode_observation_scope(snapshot.scope));
    write_file(root / "metadata.json", addon::serialize_json_canonical(metadata));
    const auto sealed = addon::seal_usage_cache_candidate(request.cache_selection->candidate_root,
        request.context.profile_storage->root, request.context.account_context, id, std::string(k_cache_format),
        [&] { return context.cancel_requested(); }, &failure);
    require(sealed.has_value(), failure.reason_code);
    snapshot.reference = *sealed;
    return snapshot;
}

void emit_page(const Snapshot& snapshot, const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    std::size_t start = 0;
    if (request.cursor) {
        try {
            start = std::stoull(*request.cursor);
        }
        catch (const std::exception&) {
            throw Usage_error("vibe_usage_cursor_invalid");
        }
        require(std::to_string(start) == *request.cursor && start > 0 && start < snapshot.records.size(),
            "vibe_usage_cursor_invalid");
    }
    addon::Usage_scan_page page;
    page.scope = snapshot.scope;
    page.snapshot_id = snapshot.reference.snapshot_id;
    page.incomplete_historical_coverage = true;
    page.cache_candidate = snapshot.reference;
    page.account_context = request.context.account_context;
    page.profile_storage = request.context.profile_storage;
    auto record = snapshot.records.begin();
    std::advance(record, static_cast<std::ptrdiff_t>(start));
    std::size_t bytes = 0;
    std::size_t next = start;
    while (record != snapshot.records.end()) {
        check_cancel(context);
        if (!addon::append_usage_page_record(page, record->second.usage, bytes, 3, 512)) {
            break;
        }
        ++record;
        ++next;
    }
    require(record == snapshot.records.end() || !page.records.empty(), "vibe_usage_record_too_large");
    page.complete = record == snapshot.records.end();
    if (!page.complete) {
        page.next_cursor = std::to_string(next);
    }
    context.succeed(addon::encode_usage_scan_result(page));
}

} // namespace

void handle_usage_scan(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    if (request->context.account_context) {
        context.fail(addon::Result_status::UNAVAILABLE, "vibe_usage_account_unavailable");
        return;
    }
    try {
        check_cancel(context);
        Snapshot snapshot = request->snapshot_id ? load_snapshot(*request, context) : create_snapshot(*request, context);
        emit_page(snapshot, *request, context);
    }
    catch (const Usage_error& error) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
            error.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "vibe_usage_failed");
    }
}

void handle_usage_session(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_session_describe_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto records = addon::decode_usage_session_request(context.request().payload, &failure);
    if (!records) {
        context.fail(addon::Result_status::FAILED, failure.reason_code);
        return;
    }
    std::vector<addon::Usage_session_description> descriptions;
    for (const auto& record : *records) {
        std::optional<addon::Usage_session_fact> fact;
        if (record.session_key && record.record_id == record_id(*record.session_key)) {
            // Counters include completed calls within a still-running turn.
            fact = addon::Usage_session_fact{*record.session_key, false};
        }
        descriptions.push_back({record, std::move(fact)});
    }
    context.succeed(addon::encode_usage_session_result(descriptions));
}

} // namespace vibe
