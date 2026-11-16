#include "vibe_export.h"
#include "vibe_projection_fixture.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/package_source.h>

#include <iostream>
#include <iterator>

namespace {
using namespace vibe_test;

addon::Json_value entry(std::string_view type, std::string_view id)
{
    auto result = addon::Json_value::object();
    result.set("type", addon::Json_value(type));
    result.set("id", addon::Json_value(id));
    result.set("sessionId", addon::Json_value("root-session"));
    result.set("generationStatus", addon::Json_value("completed"));
    return result;
}

addon::Json_value message(std::string_view role, std::string_view id, std::string_view text)
{
    auto result = entry("message", id);
    result.set("role", addon::Json_value(role));
    auto block = addon::Json_value::object();
    block.set("type", addon::Json_value("text"));
    block.set("text", addon::Json_value(text));
    auto content = addon::Json_value::array();
    content.push_back(std::move(block));
    result.set("content", std::move(content));
    return result;
}

addon::Conversation_export_request request(const Store_fixture& store,
    addon::Conversation_export_format format = addon::Conversation_export_format::MARKDOWN, bool compact = false)
{
    static unsigned unique = 0;
    const auto output = store.root / ("export-" + std::to_string(++unique));
    const auto home = store.root / "vibe-home";
    fs::create_directories(output);
    fs::create_directories(home);
    auto locator = addon::Json_value::object();
    locator.set("version", addon::Json_value(1));
    locator.set("provider_session_id", addon::Json_value("root-session"));
    locator.set("vibe_home", addon::Json_value(addon::path_to_utf8(home)));
    locator.set("transcript_path", addon::Json_value(addon::path_to_utf8(store.transcript)));
    addon::Conversation_export_request result;
    result.provider_session_id = "root-session";
    result.resume_record = std::move(locator);
    result.output_directory = addon::path_to_utf8(output);
    result.launch_environment = {addon::host_platform_id(), addon::Json_value::object(), std::nullopt};
    result.launch_environment.effective_environment.set("VIBE_HOME", addon::Json_value(addon::path_to_utf8(home)));
    result.format = format;
    result.compact = compact;
    result.profile_storage = addon::Profile_storage_context{1, addon::path_to_utf8(store.root / "profile")};
    return result;
}

std::string read(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "export file is unavailable");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::pair<addon::Conversation_export_result, std::string> exported(const addon::Conversation_export_request& input)
{
    addon::Contract_failure failure;
    const auto result = vibe::export_native_conversation(input, [] { return false; }, &failure);
    if (!result) {
        throw std::runtime_error("export failed: " + failure.reason_code + ": " + failure.detail);
    }
    const auto validated = addon::decode_conversation_export_result(addon::encode_conversation_export_result(*result), &failure);
    require(validated.has_value(), "export result failed the SDK contract");
    require(result->profile_storage && result->profile_storage->root == input.profile_storage->root,
        "export did not preserve profile acknowledgment");
    const auto body = read(addon::path_from_utf8(input.output_directory) / result->file_name);
    require(body.size() == result->bytes, "export descriptor does not describe its artifact");
    return {*result, body};
}

void native_rendering()
{
    Store_fixture store;
    store.publish(runtime());
    auto user = message("user", "user-1", "Question Ω 界\nwith **Markdown**");
    user.find("content")->push_back(json(R"({"type":"image","attachment":{"source":{"kind":"inline","data":"BINARY_MUST_NOT_APPEAR"},"alias":"photo","mimeType":"image/png"}})"));
    user.find("content")->push_back(json(R"({"type":"resource","resource":{"kind":"text","uri":"file:///not-read.txt","text":"Embedded resource text"}})"));
    auto reasoning = entry("reasoning", "reasoning-1");
    reasoning.set("text", addon::Json_value("Reasoning detail"));
    reasoning.set("summary", json(R"(["Reasoning summary"])"));
    auto effect = entry("effect", "effect-1");
    effect.set("title", addon::Json_value("read_file"));
    effect.set("detail", json(R"({"kind":"tool","toolName":"read_file","input":{"path":"/not-read.txt"}})"));
    effect.set("state", json(R"({"status":"completed","output":{"content":[{"type":"text","text":"Tool result literal fence"},{"type":"resource","resource":{"uri":"file:///binary","blob":"BINARY_MUST_NOT_APPEAR","mimeType":"application/octet-stream"}}]},"outputText":"duplicate text must not appear"})"));
    effect.find("state")->find("output")->find("content")->as_array().front().set("text",
        addon::Json_value("Tool result\n" + std::string(4, char(96)) + "\nliteral fence"));
    auto answer = message("assistant", "assistant-1", "Answer Ω\n\n::: user\nliteral text");
    auto discarded = message("assistant", "assistant-2", "Discarded fragment");
    discarded.set("outcome", json(R"({"type":"discarded","cause":"interrupt"})"));
    auto notice = entry("notice", "notice-1");
    notice.set("message", addon::Json_value("Native notice"));
    auto question = entry("callback", "callback-1");
    question.set("title", addon::Json_value("User input required"));
    question.set("detail", json(R"({"kind":"user_input","request":{"question":"Choose a color"}})"));
    question.set("state", json(R"({"status":"answered","output":{"answer":"blue"}})"));
    auto first = addon::Json_value::array();
    first.push_back(user);
    first.push_back(reasoning);
    auto second = addon::Json_value::array();
    second.push_back(effect);
    second.push_back(answer);
    second.push_back(answer);
    second.push_back(discarded);
    second.push_back(notice);
    second.push_back(question);
    publish_projection(store, projection(), {first, second});
    const auto original_pointer = read(store.transcript / "CURRENT");
    for (const auto format : {addon::Conversation_export_format::TEXT,
        addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::CONVERSATION})
    {
        const auto [result, body] = exported(request(store, format));
        require(result.message_count == 7 && result.attachment_count == 3 && !result.incomplete_tail_omitted,
            "native ordering, duplicate suppression, or attachment evidence changed");
        require(body.find("Question Ω 界") < body.find("Tool result") && body.find("Tool result") < body.find("Answer Ω") &&
            body.find("Embedded resource text") != std::string::npos && body.find("Discarded assistant content") != std::string::npos,
            "export lost native text, status, or message/block order");
        require(body.find("BINARY_MUST_NOT_APPEAR") == std::string::npos &&
            body.find("duplicate text must not appear") == std::string::npos &&
            body.find("binary content not copied") != std::string::npos,
            "export copied attachment bytes or duplicated a rendered tool result");
        const auto [compact_result, compact_body] = exported(request(store, format, true));
        require(compact_result.compact && compact_result.message_count == 3 && compact_result.attachment_count == 2 &&
            compact_body.find("Question Ω 界") != std::string::npos && compact_body.find("Answer Ω") != std::string::npos &&
            compact_body.find("blue") != std::string::npos && compact_body.find("Tool result") == std::string::npos &&
            compact_body.find("Reasoning detail") == std::string::npos && compact_body.find("Discarded fragment") == std::string::npos,
            "compact export lost discussion or retained known internal/discarded content");
    }
    require(read(store.transcript / "CURRENT") == original_pointer, "export modified native history");
}

void rejected_sources_and_tail()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    auto entries = addon::Json_value::array();
    entries.push_back(message("user", "user-1", "Complete question"));
    auto partial = message("assistant", "assistant-1", "Unfinished answer");
    partial.set("generationStatus", addon::Json_value("in_progress"));
    entries.push_back(partial);
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    const auto [result, body] = exported(request(store));
    require(result.message_count == 1 && result.incomplete_tail_omitted && body.find("Unfinished") == std::string::npos,
        "unfinished native tail became a completed answer");
    entries.push_back(message("user", "user-2", "Later accepted input"));
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    auto input = request(store);
    addon::Contract_failure failure;
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_unsettled_history" && fs::is_empty(addon::path_from_utf8(input.output_directory)),
        "export silently dropped completed discussion following unsettled native work");
    entries = addon::Json_value::array();
    entries.push_back(message("user", "user-1", "Complete question"));
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    input = request(store);
    write(store.transcript / "journal/0000000000000002.jsonl", "later completed native records\n");
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_journal_pending" && fs::is_empty(addon::path_from_utf8(input.output_directory)),
        "lagging native generation was exported as current complete discussion");
    write(store.transcript / "journal/0000000000000002.jsonl", "");
    input = request(store);
    input.provider_session_id = "other-session";
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_source_unavailable", "export selected a different native conversation");
    input = request(store);
    input.launch_environment.effective_environment.set("VIBE_HOME", addon::Json_value(addon::path_to_utf8(store.root / "other-home")));
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_home_mismatch", "export ignored the frozen source home");
    entries.push_back(message("assistant", "user-1", "Conflicting duplicate"));
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    input = request(store);
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_duplicate_conflict" && fs::is_empty(addon::path_from_utf8(input.output_directory)),
        "conflicting native duplicate produced a partial artifact");
}

void structured_tool_results()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    auto entries = addon::Json_value::array();
    auto effect = entry("effect", "structured-only");
    effect.set("title", addon::Json_value("agents.list"));
    effect.set("detail", json(R"({"kind":"tool","toolName":"agents.list","input":{}})"));
    effect.set("state", json(R"({"status":"completed","output":{"content":[],"structured_content":{"agents":[{"name":"native-explorer","status":"running"}]}},"outputText":""})"));
    entries.push_back(effect);
    effect.set("id", addon::Json_value("mixed-output"));
    effect.set("state", json(R"({"status":"completed","output":{"content":[{"type":"text","text":"Native textual result"}],"structured_content":{"structured_marker":"Native structured result"}},"outputText":"duplicate fallback"})"));
    entries.push_back(effect);
    effect.set("id", addon::Json_value("text-fallback"));
    effect.set("state", json(R"({"status":"completed","output":{"content":[]},"outputText":"Native outputText fallback"})"));
    entries.push_back(effect);
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    for (const auto format : {addon::Conversation_export_format::TEXT,
        addon::Conversation_export_format::MARKDOWN, addon::Conversation_export_format::CONVERSATION})
    {
        const auto [result, body] = exported(request(store, format));
        require(result.message_count == 3 && body.find("native-explorer") != std::string::npos &&
            body.find("Native textual result") != std::string::npos &&
            body.find("Native structured result") != std::string::npos &&
            body.find("Native outputText fallback") != std::string::npos &&
            body.find("duplicate fallback") == std::string::npos,
            "native structured tool output or text fallback was lost or duplicated");
    }
}

void discarded_history_roles()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    auto user = message("user", "skipped-user", "Keep this skipped user discussion");
    user.set("source", addon::Json_value("turn_start"));
    user.set("turnId", addon::Json_value("turn-1"));
    user.set("outcome", json(R"({"type":"discarded","cause":"skipped"})"));
    auto reasoning = entry("reasoning", "discarded-reasoning");
    reasoning.set("text", addon::Json_value("Interrupted internal reasoning"));
    reasoning.set("outcome", json(R"({"type":"discarded","cause":"interrupt"})"));
    auto entries = addon::Json_value::array();
    entries.push_back(user);
    entries.push_back(reasoning);
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    const auto [full, body] = exported(request(store));
    require(full.message_count == 2 && body.find("Keep this skipped user discussion") != std::string::npos &&
        body.find("Skipped user message") != std::string::npos &&
        body.find("Discarded reasoning") != std::string::npos &&
        body.find("Discarded assistant content") == std::string::npos,
        "discarded native user or reasoning content was mislabeled");
    const auto [compact, discussion] = exported(request(store, addon::Conversation_export_format::MARKDOWN, true));
    require(compact.message_count == 1 && discussion.find("Keep this skipped user discussion") != std::string::npos &&
        discussion.find("Interrupted internal reasoning") == std::string::npos,
        "compact export discarded user discussion or retained interrupted reasoning");
}

void large_text_and_cancellation()
{
    Store_fixture store;
    store.publish(runtime());
    auto value = projection();
    auto entries = addon::Json_value::array();
    const auto large = std::string(2 * 1024 * 1024, 'x') + "\nEnd of large native message";
    entries.push_back(message("assistant", "assistant-large", large));
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    const auto [result, body] = exported(request(store, addon::Conversation_export_format::TEXT));
    require(result.bytes > 2 * 1024 * 1024 && body.find("End of large native message") != std::string::npos,
        "native conversation text was truncated to IPC bounds");
    auto input = request(store);
    addon::Contract_failure failure;
    unsigned calls = 0;
    require(!vibe::export_native_conversation(input, [&] { return ++calls >= 3; }, &failure) &&
        fs::is_empty(addon::path_from_utf8(input.output_directory)), "cancelled export retained a partial artifact");
    entries = addon::Json_value::array();
    auto unsupported = entry("future-entry", "future-1");
    entries.push_back(unsupported);
    value.find("snapshot")->find("history")->set("entries", entries);
    publish_projection(store, value);
    input = request(store);
    require(!vibe::export_native_conversation(input, {}, &failure) &&
        failure.reason_code == "vibe_export_entry_unsupported" && fs::is_empty(addon::path_from_utf8(input.output_directory)),
        "unknown history was silently omitted");
}
} // namespace

int main()
{
    try {
        native_rendering();
        structured_tool_results();
        discarded_history_roles();
        rejected_sources_and_tail();
        large_text_and_cancellation();
        std::cout << "Vibe native export tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
