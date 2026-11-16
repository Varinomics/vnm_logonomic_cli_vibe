#include "vibe_config.h"

#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace addon = logonomic::cli_addon;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void rejects(const std::string& original, const char* reason)
{
    try {
        (void)vibe::updated_hooks(original);
    } catch (const std::runtime_error& error) {
        require(error.what() == std::string(reason), "wrong config failure reason");
        return;
    }
    throw std::runtime_error("unsafe configuration accepted");
}

void fresh_and_idempotent()
{
    for (const auto platform : {vibe::Hook_platform::posix, vibe::Hook_platform::windows}) {
        const auto output = vibe::updated_hooks({}, platform);
        const auto document = toml::parse(output);
        const auto* hooks = document["hooks"].as_array();
        require(hooks && hooks->size() == 4, "four lifecycle hooks required");
        for (const auto& value : *hooks) {
            const auto& hook = *value.as_table();
            const auto event = hook["type"].value<std::string>();
            require(event.has_value(), "hook type missing");
            require(hook["command"].value<std::string>() == vibe::hook_command(platform),
                "wrong hook command");
            require(hook["timeout"].value<double>() == 5, "unbounded hook timeout");
            if (*event == "pre_agent")
                require(hook["match"].value<std::string>() == "main", "subagent pre_agent hook enabled");
            else
                require(!hook.contains("match"), "lifecycle hook unexpectedly filtered");
        }
        require(vibe::updated_hooks(output, platform) == output, "transform is not idempotent");
    }
}

void preserves_user_syntax()
{
    const std::string original = "\xEF\xBB\xBF# Ευχαριστώ\r\n"
        "[[hooks]] # user-owned audit\r\n"
        "name='audit'\r\ntype = 'post_tool'\r\n"
        "command = '''echo Ω\r\n"
        "# Logonomic managed Vibe hook: session_start\r\n"
        "[[hooks]]\r\nname = 'logonomic-vibe-session-start'\r\n'''\r\n";
    const auto output = vibe::updated_hooks(original);
    require(output.starts_with(original), "user bytes were rewritten");
    require(vibe::updated_hooks(output) == output, "BOM/Unicode/CRLF damaged owned offsets");
    require(output.find("[[hooks]]\r\nname = \"logonomic-vibe-session-start\"") != std::string::npos,
        "new hook did not preserve CRLF style");

    const auto literal = vibe::hook_entry("session_start");
    const auto with_lookalike = "note = '''\n" + literal + "'''\n";
    require(toml::parse(vibe::updated_hooks(with_lookalike))["hooks"].as_array()->size() == 4,
        "multiline string claimed ownership");
}

void updates_only_owned_values()
{
    auto original = vibe::hook_entry("pre_agent", vibe::Hook_platform::posix);
    original.replace(original.find("timeout = 5"), 11, "timeout = 29 # keep this deadline comment");
    original.replace(original.find("match = \"main\""), 14, "match = 'other' # keep matching comment");
    original += "\n[[hooks]] # my next table\nname='mine'\ntype='notification'\ncommand='echo λ'\n";
    const auto output = vibe::updated_hooks(original, vibe::Hook_platform::windows);
    require(output.find("timeout = 5 # keep this deadline comment") != std::string::npos,
        "owned deadline comment lost");
    require(output.find("match = \"main\" # keep matching comment") != std::string::npos,
        "owned matcher comment lost");
    require(output.find("[[hooks]] # my next table\nname='mine'\ntype='notification'\ncommand='echo λ'\n") !=
        std::string::npos, "neighboring user table changed");
    require(vibe::updated_hooks(output, vibe::Hook_platform::windows) == output,
        "owned replacements not idempotent");
}

void inline_hook_arrays()
{
    std::size_t case_number = 0;
    for (const std::string original : {
        "hooks = [] # empty user array\n",
        "hooks = [{ name='my-audit',type='post_agent',command='echo Ω'}]\n",
        "hooks = [\n{ name='my-audit',type='post_agent',command='echo Ω'}, # keep,\n]\n",
        "hooks = [\n{ name='my-audit',type='post_agent',command='echo Ω'} # comma in a comment,\n]\n"}) {
        ++case_number;
        std::string output;
        try { output = vibe::updated_hooks(original); }
        catch (const std::exception& error) {
            throw std::runtime_error("inline case " + std::to_string(case_number) + ": " + error.what());
        }
        const auto document = toml::parse(output);
        require(document["hooks"].as_array()->size() == (original.starts_with("hooks = []") ? 4u : 5u),
            "inline user hooks lost");
        require(vibe::updated_hooks(output) == output, "inline hook transform not idempotent");
        if (original.find("my-audit") != std::string::npos)
            require(output.find("{ name='my-audit',type='post_agent',command='echo Ω'}") !=
                std::string::npos, "inline user syntax changed");
    }
}

void malformed_and_conflicting_hooks()
{
    rejects("hooks = [", "vibe_hooks_invalid_toml");
    rejects("hooks = 'not an array'", "vibe_hooks_array_required");
    rejects("hooks = [1]", "vibe_hook_table_required");
    rejects("[[hooks]]\nname='logonomic-vibe-session-start'\ntype='session_start'\ncommand='mine'\n",
        "vibe_hook_name_collision");
    rejects(vibe::hook_entry("pre_agent") + "strict = true\n", "vibe_managed_hook_modified");
    rejects(vibe::hook_entry("session_end") + vibe::hook_entry("session_end"),
        "vibe_managed_hook_duplicate");
    // The marker alone cannot claim a different table or swallow a user hook.
    rejects("# Logonomic managed Vibe hook: session_start\n[[hooks]]\nname='mine'\n"
        "type='session_start'\ncommand='echo hi'\ntimeout=5\n", "vibe_managed_hook_modified");
}

void windows_command_is_shell_neutral()
{
    constexpr std::string_view prefix =
        "powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand ";
    const auto command = vibe::hook_command(vibe::Hook_platform::windows);
    require(command.starts_with(prefix), "Windows command has shell-dependent quoting");
    const auto decoded = addon::decode_base64(command.substr(prefix.size()));
    require(decoded && decoded->size() % 2 == 0, "PowerShell command isn't UTF-16LE");
    std::string script;
    for (std::size_t i = 0; i < decoded->size(); i += 2) {
        require((*decoded)[i + 1] == '\0', "PowerShell command contains unexpected non-ASCII code");
        script += (*decoded)[i];
    }
    require(script == "$ProgressPreference = 'SilentlyContinue'; "
        "if ($env:VNM_VIBE_HOOK -and (Test-Path -LiteralPath $env:VNM_VIBE_HOOK -PathType Leaf)) "
        "{ & $env:VNM_VIBE_HOOK; exit $LASTEXITCODE }", "PowerShell guard or stdin forwarding changed");
}

class Environment {
public:
    Environment(const char* name, const std::string& value) : m_name(name)
    {
        const char* old = std::getenv(name);
        m_present = old != nullptr;
        if (old) m_previous = old;
        set(value);
    }
    ~Environment()
    {
        if (m_present) set(m_previous);
#ifdef _WIN32
        else _putenv_s(m_name, "");
#else
        else unsetenv(m_name);
#endif
    }

private:
    void set(const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(m_name, value.c_str());
#else
        setenv(m_name, value.c_str(), 1);
#endif
    }
    const char* m_name;
    bool m_present = false;
    std::string m_previous;
};

std::string shell_quote(const fs::path& path)
{
    const auto text = addon::path_to_utf8(path);
#ifdef _WIN32
    return '"' + text + '"';
#else
    std::string result = "'";
    for (const char character : text) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + '\'';
#endif
}

std::string read_bytes(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void actual_shell_preserves_stdin(const char* executable)
{
    const auto directory = fs::temp_directory_path() /
        ("vibe-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
    } cleanup{directory};
#ifdef _WIN32
    const auto helper = directory / "hook with ' [brackets] & spaces.exe";
#else
    const auto helper = directory / "hook with ' [brackets] & spaces";
#endif
    fs::copy_file(fs::absolute(addon::path_from_utf8(executable)), helper);
    const auto input = directory / "input.json";
    const auto output = directory / "output.json";
    const auto error = directory / "stderr.txt";
    const std::string payload = "{\"cwd\":\"/tmp/Ελληνικά & spaces\",\"hook_event_name\":\"session_start\"}\r\n";
    { std::ofstream stream(input, std::ios::binary); stream << payload; }
    Environment probe("VNM_VIBE_CONFIG_STDIN_PROBE", "1");
    Environment helper_path("VNM_VIBE_HOOK", addon::path_to_utf8(helper));
    const auto command = vibe::hook_command() + " < " + shell_quote(input) +
        " > " + shell_quote(output) + " 2> " + shell_quote(error);
    require(std::system(command.c_str()) == 0, "guarded native helper did not run");
    require(read_bytes(output) == payload, "hook command changed or consumed stdin bytes");
    require(read_bytes(error).empty(), "hook command emitted diagnostics on success");
    {
        Environment no_host("VNM_VIBE_HOOK", "");
        require(std::system(command.c_str()) == 0, "hook failed outside Logonomic");
        require(read_bytes(output).empty(), "hook ran without Logonomic environment");
        require(read_bytes(error).empty(), "standalone Vibe would get spurious warnings");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (const char* probe = std::getenv("VNM_VIBE_CONFIG_STDIN_PROBE");
        probe && std::string_view(probe) == "1") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        std::cout << std::cin.rdbuf();
        return 0;
    }
    const char* current_case = "fresh_and_idempotent";
    try {
        fresh_and_idempotent();
        current_case = "preserves_user_syntax";
        preserves_user_syntax();
        current_case = "updates_only_owned_values";
        updates_only_owned_values();
        current_case = "inline_hook_arrays";
        inline_hook_arrays();
        current_case = "malformed_and_conflicting_hooks";
        malformed_and_conflicting_hooks();
        current_case = "windows_command_is_shell_neutral";
        windows_command_is_shell_neutral();
        require(argc > 0, "test executable path missing");
        current_case = "actual_shell_preserves_stdin";
        actual_shell_preserves_stdin(argv[0]);
        std::cout << "Vibe managed-configuration tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << current_case << ": " << error.what() << '\n';
        return 1;
    }
}
