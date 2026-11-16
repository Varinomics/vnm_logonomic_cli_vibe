#include "vibe_config.h"

#include <logonomic_cli_addon/session_contracts.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vibe {
namespace {

constexpr std::array<std::string_view, 4> events = {
    "session_start", "pre_agent", "notification", "session_end"};
constexpr std::string_view marker_prefix = "# Logonomic managed Vibe hook: ";

std::string hook_name(std::string_view event)
{
    std::string name = "logonomic-vibe-" + std::string(event);
    std::replace(name.begin(), name.end(), '_', '-');
    return name;
}

std::string toml_string(std::string_view value)
{
    std::string result = "\"";
    for (const char character : value) {
        if (character == '\\' || character == '"') result += '\\';
        if (character == '\r') result += "\\r";
        else if (character == '\n') result += "\\n";
        else result += character;
    }
    return result + '"';
}

struct Line {
    std::string_view text;
    std::size_t next;
};

Line line_at(std::string_view source, std::size_t offset)
{
    const auto newline = source.find('\n', offset);
    auto end = newline == std::string_view::npos ? source.size() : newline;
    if (end > offset && source[end - 1] == '\r') --end;
    return {source.substr(offset, end - offset),
        newline == std::string_view::npos ? source.size() : newline + 1};
}

// toml++ columns count Unicode codepoints. A BOM is not a column, and CR is
// a column whereas LF starts the next line. Byte offsets must agree with it.
class Source_offsets {
public:
    explicit Source_offsets(const std::string& source) : m_source(source)
    {
        m_lines.push_back(source.starts_with("\xEF\xBB\xBF") ? 3 : 0);
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\n') m_lines.push_back(i + 1);
        }
    }

    std::size_t byte(toml::source_position position) const
    {
        if (!position || position.line > m_lines.size())
            throw std::runtime_error("vibe_hooks_source_region_invalid");
        auto offset = m_lines[position.line - 1];
        for (std::size_t column = 1; column < position.column; ++column) {
            if (offset >= m_source.size() || m_source[offset] == '\n')
                throw std::runtime_error("vibe_hooks_source_region_invalid");
            ++offset;
            while (offset < m_source.size() &&
                (static_cast<unsigned char>(m_source[offset]) & 0xC0) == 0x80) ++offset;
        }
        return offset;
    }

private:
    const std::string& m_source;
    std::vector<std::size_t> m_lines;
};

void string_regions(const toml::node& node, const Source_offsets& offsets,
    std::vector<std::pair<std::size_t, std::size_t>>& regions)
{
    if (node.is_string()) {
        regions.emplace_back(offsets.byte(node.source().begin), offsets.byte(node.source().end));
    } else if (const auto* table = node.as_table()) {
        for (const auto& [key, value] : *table) {
            (void)key;
            string_regions(value, offsets, regions);
        }
    } else if (const auto* array = node.as_array()) {
        for (const auto& value : *array) string_regions(value, offsets, regions);
    }
}

bool inside_string(std::size_t offset,
    const std::vector<std::pair<std::size_t, std::size_t>>& regions)
{
    const auto after = std::upper_bound(regions.begin(), regions.end(), offset,
        [](std::size_t position, const auto& region) { return position < region.first; });
    return after != regions.begin() && offset < std::prev(after)->second;
}

std::size_t following_table(std::string_view source, std::size_t cursor)
{
    while (cursor < source.size()) {
        const auto line = line_at(source, cursor);
        const auto first = line.text.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const auto stripped = line.text.substr(first);
            if (stripped.starts_with(marker_prefix)) return std::string::npos;
            if (!stripped.starts_with('#')) return cursor + first;
        }
        cursor = line.next;
    }
    return std::string::npos;
}

struct Edit {
    std::size_t begin;
    std::size_t end;
    std::string replacement;
};

void replace_value(std::vector<Edit>& edits, const Source_offsets& offsets,
    const toml::node& value, std::string replacement)
{
    edits.push_back({offsets.byte(value.source().begin), offsets.byte(value.source().end),
        std::move(replacement)});
}

std::string entry(std::string_view event, Hook_platform platform,
    bool inline_table, std::string_view newline)
{
    const std::string separator = inline_table ? ", " : std::string(newline);
    std::string result = std::string(marker_prefix) + std::string(event) + std::string(newline);
    result += inline_table ? "{ " : "[[hooks]]" + std::string(newline);
    result += "name = " + toml_string(hook_name(event)) + separator;
    result += "type = " + toml_string(event) + separator;
    result += "command = " + toml_string(hook_command(platform)) + separator;
    result += "timeout = 5";
    if (event == "pre_agent") result += separator + "match = \"main\"";
    result += inline_table ? " }" : std::string(newline);
    return result;
}

toml::table parse(std::string_view source)
{
    try {
        return toml::parse(source);
    } catch (const toml::parse_error&) {
        // The host snapshot can contain private configuration; don't expose
        // parser diagnostics, which include excerpts of those bytes.
        throw std::runtime_error("vibe_hooks_invalid_toml");
    }
}

} // namespace

std::string hook_command(Hook_platform platform)
{
    if (platform == Hook_platform::posix) {
        return "if [ -n \"$VNM_VIBE_HOOK\" ] && [ -x \"$VNM_VIBE_HOOK\" ]; then "
            "exec \"$VNM_VIBE_HOOK\"; fi";
    }
    // Vibe can use cmd, Git Bash or PowerShell on native Windows. Keeping all
    // outer-shell arguments plain ASCII avoids incompatible quoting rules.
    // The native helper inherits stdin; PowerShell does not parse the payload.
    // Windows PowerShell serializes progress records, such as first-use module
    // preparation for Test-Path, onto a redirected stderr as CLIXML. Silence
    // progress in this process only; errors and the helper's streams remain.
    constexpr std::string_view script =
        "$ProgressPreference = 'SilentlyContinue'; "
        "if ($env:VNM_VIBE_HOOK -and (Test-Path -LiteralPath $env:VNM_VIBE_HOOK -PathType Leaf)) "
        "{ & $env:VNM_VIBE_HOOK; exit $LASTEXITCODE }";
    std::string utf16;
    utf16.reserve(script.size() * 2);
    for (const char character : script) {
        utf16 += character;
        utf16 += '\0';
    }
    return "powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand " +
        logonomic::cli_addon::encode_base64(utf16);
}

std::string hook_entry(std::string_view event, Hook_platform platform)
{
    if (std::find(events.begin(), events.end(), event) == events.end())
        throw std::invalid_argument("vibe_hook_event_unknown");
    return entry(event, platform, false, "\n");
}

std::string updated_hooks(std::string original, Hook_platform platform)
{
    const auto document = parse(original);
    const Source_offsets offsets(original);
    std::vector<std::pair<std::size_t, std::size_t>> strings;
    string_regions(document, offsets, strings);
    std::sort(strings.begin(), strings.end());

    const auto* hooks = document["hooks"].as_array();
    if (document.contains("hooks") && !hooks)
        throw std::runtime_error("vibe_hooks_array_required");
    std::map<std::size_t, const toml::table*> tables;
    if (hooks) {
        for (const auto& hook : *hooks) {
            const auto* table = hook.as_table();
            if (!table) throw std::runtime_error("vibe_hook_table_required");
            tables.emplace(offsets.byte(table->source().begin), table);
        }
    }

    std::map<std::string_view, const toml::table*> owned;
    for (std::size_t cursor = original.starts_with("\xEF\xBB\xBF") ? 3 : 0;
         cursor < original.size();) {
        const auto line = line_at(original, cursor);
        if (line.text.starts_with(marker_prefix) && !inside_string(cursor, strings)) {
            const auto event = line.text.substr(marker_prefix.size());
            if (std::find(events.begin(), events.end(), event) != events.end()) {
                const auto table = tables.find(following_table(original, line.next));
                if (table != tables.end()) {
                    if (!owned.emplace(event, table->second).second)
                        throw std::runtime_error("vibe_managed_hook_duplicate");
                }
            }
        }
        cursor = line.next;
    }

    std::vector<Edit> edits;
    std::vector<std::string_view> missing;
    const std::string expected_command = hook_command(platform);
    for (const auto event : events) {
        const auto managed = owned.find(event);
        const auto* selected = managed == owned.end() ? nullptr : managed->second;
        for (const auto& [position, table] : tables) {
            (void)position;
            if (table != selected && table->get_as<std::string>("name") &&
                table->at("name").value<std::string>() == hook_name(event))
                throw std::runtime_error("vibe_hook_name_collision");
        }
        if (!selected) {
            missing.push_back(event);
            continue;
        }
        const bool main_only = event == "pre_agent";
        const auto timeout = (*selected)["timeout"].value<double>();
        if (selected->size() != (main_only ? 5u : 4u) ||
            (*selected)["name"].value<std::string>() != hook_name(event) ||
            (*selected)["type"].value<std::string>() != event ||
            !(*selected)["command"].is_string() || !timeout ||
            (main_only && !(*selected)["match"].is_string()))
            throw std::runtime_error("vibe_managed_hook_modified");
        if ((*selected)["command"].value<std::string>() != expected_command)
            replace_value(edits, offsets, *selected->get("command"), toml_string(expected_command));
        if (*timeout != 5)
            replace_value(edits, offsets, *selected->get("timeout"), "5");
        if (main_only && (*selected)["match"].value<std::string>() != "main")
            replace_value(edits, offsets, *selected->get("match"), "\"main\"");
    }

    const auto first_newline = original.find('\n');
    const std::string_view newline = first_newline != std::string::npos && first_newline > 0 &&
        original[first_newline - 1] == '\r' ? "\r\n" : "\n";
    if (!missing.empty() && hooks && (hooks->empty() || hooks->front().as_table()->is_inline())) {
        const auto closing = offsets.byte(hooks->source().end) - 1;
        if (original.at(closing) != ']') throw std::runtime_error("vibe_hooks_source_region_invalid");
        bool comma_at_closing = false;
        if (!hooks->empty()) {
            const auto last_end = offsets.byte(hooks->back().source().end);
            auto cursor = last_end;
            while (cursor < closing) {
                if (original[cursor] == '#') cursor = line_at(original, cursor).next;
                else if (original[cursor] == ' ' || original[cursor] == '\t' ||
                         original[cursor] == '\r' || original[cursor] == '\n') ++cursor;
                else break;
            }
            if (cursor == closing) {
                if (last_end == closing) comma_at_closing = true;
                else edits.push_back({last_end, last_end, ","});
            }
        }
        std::string appended = comma_at_closing ? "," : "";
        appended += newline;
        for (const auto event : missing)
            appended += entry(event, platform, true, newline) + ',' + std::string(newline);
        edits.push_back({closing, closing, std::move(appended)});
    } else if (!missing.empty()) {
        std::string appended;
        if (!original.empty() && original.back() != '\n') appended += newline;
        for (const auto event : missing)
            appended += std::string(newline) + entry(event, platform, false, newline);
        edits.push_back({original.size(), original.size(), std::move(appended)});
    }

    std::sort(edits.begin(), edits.end(), [](const auto& left, const auto& right) {
        return left.begin > right.begin;
    });
    auto boundary = original.size();
    for (const auto& edit : edits) {
        if (edit.begin > edit.end || edit.end > boundary)
            throw std::runtime_error("vibe_hooks_edit_overlap");
        original.replace(edit.begin, edit.end - edit.begin, edit.replacement);
        boundary = edit.begin;
    }
    (void)parse(original);
    return original;
}

} // namespace vibe
