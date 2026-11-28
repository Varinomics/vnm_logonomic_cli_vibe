#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace vibe {

enum class Hook_platform { posix, windows };

#ifdef _WIN32
inline constexpr Hook_platform native_hook_platform = Hook_platform::windows;
#else
inline constexpr Hook_platform native_hook_platform = Hook_platform::posix;
#endif

// The launcher supplies VNM_VIBE_HOOK only to its own Vibe process. Persistent
// user hooks therefore do nothing when Vibe is started outside Logonomic.
std::string hook_command(Hook_platform platform = native_hook_platform);
std::string hook_entry(std::string_view event,
    Hook_platform platform = native_hook_platform);

// Checks the native discovery directory (VIBE_HOME or a project's .vibe).
// The managed relay is inert when the task clears VNM_VIBE_HOOK.
bool allows_text_authoring(const std::filesystem::path& directory);

// Transform the host's bounded hooks.toml snapshot; never read or write files.
// Invalid TOML, a reserved-name collision or a modified owned schema throws
// without returning replacement bytes. User syntax and comments are retained.
std::string updated_hooks(std::string original,
    Hook_platform platform = native_hook_platform);

} // namespace vibe
