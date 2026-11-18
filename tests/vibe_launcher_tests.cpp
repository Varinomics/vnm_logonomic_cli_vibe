#include "vibe_launcher.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main()
{
    using vibe::interactive_invocation;
    require(interactive_invocation({"vibe"}).has_value(), "plain native launcher");
    require(interactive_invocation({"/opt/with spaces/vibe", "--agent", "ask", "initial prompt"}).has_value(), "agent and prompt");
    require(interactive_invocation({"C:\\Tools\\vibe.exe", "--resume", "session-id"}).has_value(), "exact resume");
    require(interactive_invocation({"vibe", "--resume"}).has_value(), "native resume picker");
    require(interactive_invocation({"vibe", "--", "--literal prompt"}).has_value(), "literal option-shaped prompt");
    require(!interactive_invocation({"other", "vibe"}), "unrelated command");
    require(!interactive_invocation({"vibe", "-p", "prompt"}), "headless invocation");
    require(!interactive_invocation({"vibe", "config", "list"}), "configuration command");
    require(!interactive_invocation({"vibe", "--version"}), "version command");
    require(!interactive_invocation({"vibe", "--agent"}), "missing option value");
    require(!interactive_invocation({"vibe", "--worktree"}), "workspace-changing launch");
    require(!interactive_invocation({"vibe", "--unknown"}), "unknown launch semantics");
    require(vibe::supported_version("3.1.1"), "first native-hook release");
    require(vibe::supported_version("3.1.2"), "observed current release");
    require(vibe::supported_version("4.0.0"), "future releases are not artificially capped");
    require(!vibe::supported_version("3.1.0"), "older hooks are insufficient");
    require(!vibe::supported_version("3.1.1-rc.1"), "minimum prerelease precedes hook release");
    require(!vibe::supported_version("not a version"), "unverified version");
    require(!vibe::resolve_home({{"VIBE_HOME", "~/.vibe"}}, "/repo"), "tilde selector must not target another config");
    require(!vibe::resolve_home({{"VIBE_HOME", " relative "}}, "/repo"), "selector trimming mismatch");
    require(!vibe::resolve_home({}, "/repo"), "missing platform home");
    std::cout << "Vibe launcher contracts passed\n";
}
