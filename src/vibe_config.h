#pragma once

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

// Transform the host's bounded hooks.toml snapshot; never read or write files.
// Invalid TOML, a reserved-name collision or a modified owned schema throws
// without returning replacement bytes. User syntax and comments are retained.
std::string updated_hooks(std::string original,
    Hook_platform platform = native_hook_platform);

} // namespace vibe
