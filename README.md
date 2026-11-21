# Mistral Vibe CLI add-on for Logonomic

Native Logonomic integration for an installed [Mistral Vibe](https://github.com/mistralai/mistral-vibe)
CLI, built with the [Logonomic CLI add-on SDK](https://github.com/Varinomics/vnm_logonomic_cli_addon/tree/master/sdk/logonomic_cli_addon_sdk).

**Package:** `logonomic.cli.vibe` · **Minimum Vibe:** `3.1.1`

The package discovers Vibe, prepares its native hooks, tracks the foreground
conversation and observed child runs, reports native context evidence,
scans persisted token usage, exports conversation history, and resumes a
remembered conversation by its full session ID. It contains a C++20 adapter,
a native hook helper, the SDK launch bridge, capability sidecars, an integrity
index, and dependency notices. The minimum version gates hook compatibility;
the additional native-store integrations are qualified against Vibe 3.1.2 and
fail their affected capability when the store contract is unfamiliar.

## Implemented capabilities

| Capability | Behavior |
| --- | --- |
| `presentation@1` | Mistral Vibe display name and an original package icon. |
| `launcher-discovery@1` | Finds an installed `vibe` on the supplied search path, with a `.local/bin` fallback, and recognizes supported literal interactive invocations. |
| `launch-preparation@3` | Probes the selected executable's version and proposes the environment and managed configuration required for hooks. |
| `accounts@2` | Discovers and resolves the native filesystem profile selected by a Vibe launcher, including a custom `VIBE_HOME`. |
| `account-profile-managed-configuration@1` | Binds hook installation to the selected profile retained by the host. |
| `managed-configuration@1` | Adds four owned hooks to `VIBE_HOME/hooks.toml` through the host's configuration transaction. |
| `lifecycle-events@2` | Native session start/resume/end, confirmed root working status, inferred root completion/attention hints, and continuous observation of native child runs. |
| `helper-ingress@2` | Delivers bounded native hook events through the SDK helper client and checks the host's active session ledger. |
| `lifecycle-correlation-lifetime@1` | Reserves and retires exact native activation bindings; observation pages remain replayable until acknowledged. |
| `child-state@1` | Reports the state of each observed direct child turn, including completed, interrupted and failed outcomes. |
| `model-state@1` | Reports the root model as unavailable at each native session start, because Vibe exposes only the selected alias. Owned tasks report their model as unavailable. |
| `structured-model-observation@1` | Answers a host model query during task inspection. Vibe's native headless history omits the execution model, so the query reports it as unavailable while the task still settles. |
| `execution-context@1` | Provides the native session-start directory to hosts that request it. Logonomic uses the registered conversation directory for Resume. |
| `model-context-delivery@2` | Host-owned title/summary and other model guidance through the hidden native session-start context channel, fenced to the captured activation. |
| `context-state@2` | Native context occupancy and capacity; fresh, explicitly incomplete successful-compaction history lower bounds. |
| `profile-storage@1` | Acknowledges the host-owned provider cache directory. |
| `usage-scan@3` | Verified cumulative input/output token records per native session, with immutable selected caches and bounded pages. |
| `usage-session@1` | Maps usage record keys back to exact native session IDs without claiming a completed-turn cutoff. |
| `usage-temporal@1` | Explicitly marks the available cumulative usage as unallocated by date. |
| `conversation-export@2` | Full or compact plain text, Markdown, and highlighted conversation exports from an exact retained native source. |
| `structured-session@3` | Prepares fresh implementation tasks and inspects their native streaming output through final process exit. |
| `interactive-resume@2` | Prepares the original full session ID with its native home and storage root, preserving supported launcher options. |
| `interactive-resume@3` | Reads bounded native metadata and generation witnesses to inspect remembered-session availability without modifying history. |

Authentication probes, quotas, Logonomic-launched delegated
tasks, read-only structured review, ongoing coordination delivery, paired
approvals, authoritative root turn outcomes, and mid-session worktree changes
remain unimplemented.
Usage by model and complete cache-token accounting remain unavailable from the
persisted evidence used here. Vibe 3's unified store is the source; an older
`messages.jsonl` layout is not a substitute for its accounting or transcript
contract.

The [native integration notes](docs/native_usage_and_context.md#remaining-features-and-implementation-routes)
identify concrete native interfaces and evidence gaps for the remaining work.

## Using the package

Install Vibe separately and make its executable available in the environment
Logonomic supplies to the launcher. An ordinary launcher is:

```sh
vibe
```

The adapter accepts literal interactive invocations with an initial prompt,
`--agent`, `--add-dir`, `--auto-approve`/`--yolo`, `--smart-approve`, and `--trust`.
It preserves native `--enabled-tools` and `--disabled-tools` selections, including
repeated flags, for interactive launches, exact resumes, and implementation tasks.
Exact remembered-session resume replaces an old resume/continue selector and
removes the original startup prompt so it is not submitted a second time.

Vibe selects one configuration directory per launcher. An absolute
`VIBE_HOME` selects a custom profile, and the default is the user's
`.vibe` directory. Logonomic retains the profile for hook installation,
owned tasks, resume, export and usage scans. Vibe has no catalogue of other
arbitrary directories selected by different launchers. A profile identifies
the local source containing the history. Remote sign-in and billing identity
require independent evidence.

On Windows, profile identity follows the parent directory's case-sensitivity
setting. Two distinct homes named `Work` and `work` in a case-sensitive
directory have separate grants and usage caches.

An ordinary launch that already contains `--resume`, `--continue`, or `-c` is
recognized but left unmanaged: native Vibe can restore a different working
directory. Use Logonomic's remembered-session resume for the checked path.
Headless/admin commands, compound shell programs, `uvx` wrappers, unknown flags,
and workspace-changing options such as `--workdir` and `--worktree` are also
outside managed launch support. A nonempty `VIBE_CWD` leaves an ordinary launch
unmanaged. Windows launch preparation requires a native `vibe.exe`, rather than
a `.cmd`/`.bat` shim.

The native home defaults to `$HOME/.vibe` on POSIX or `%USERPROFILE%\.vibe` on
Windows. A custom `VIBE_HOME` must be absolute, with no leading/trailing
whitespace. Relative paths and literal `~` paths are refused because the host's
managed-file selector and Vibe expand them differently.

Import or stage the **complete package directory** through Logonomic's CLI
add-on package mechanism. The package depends on a host supporting the declared
capability versions. Its helper paths and integrity index belong to one package
generation; copying only the adapter executable is insufficient.

### Managed native hooks

The host applies the adapter's pure TOML transformation to `hooks.toml`. Launch
preparation itself does not write the user's configuration. The transformation
preserves user hooks, comments, line endings, and TOML structure, and is
idempotent. A conflicting or modified reserved hook is reported as
`vibe_hooks_configuration_conflict` instead of being overwritten.

| Owned hook | Native event | Purpose |
| --- | --- | --- |
| `logonomic-vibe-session-start` | `session_start` | Capture startup, resume, clear, or fork identity, native path and cwd; request activation-bound host guidance. |
| `logonomic-vibe-pre-agent` | `pre_agent`, `match = "main"` | Observe root turn start, including a root fork, without treating subagents as the foreground conversation. |
| `logonomic-vibe-notification` | `notification` | Observe completion/failure and requests for user attention. |
| `logonomic-vibe-session-end` | `session_end` | Observe termination of the current native session. |

Commands resolve the packaged helper through the launch-specific
`VNM_VIBE_HOOK` environment variable. They become no-ops when that variable is
absent. POSIX uses a quoted shell guard; Windows uses a fixed PowerShell encoded
command so the helper path is passed literally across native shell choices.
No package path is pinned in the persistent hooks file.

The hook helper forwards only the fields used for identity, lifecycle, and
directory classification. Prompt text, responses, tool output, and
notification messages are discarded. Input is bounded to 1 MiB. Event delivery,
acknowledgment, and the optional guidance request share a 3.5-second deadline
inside Vibe's 5-second hook timeout. Malformed input or unavailable host delivery
exits quietly.

Accepted host guidance is returned only as native `additional_context`. Vibe
adds it to the next model request while keeping it out of public conversation
history. The helper preserves the user prompt and emits no visible system
message. A refused or stale activation produces no guidance and is never
retried without its activation fence. Guidance is delivered at startup, resume,
clear, or fork; this does not implement ongoing coordination delivery.

Vibe notifications are asynchronous and lack turn IDs. Completion/failure
notifications therefore produce an **inferred** return-to-input phase, and
approval/question notifications produce **inferred**, unpaired attention. An
event for a previous native session is ignored. Same-session notification
ordering and turn interruption are not fully observable in the current native
hook contract; a completion notification is not a confirmed turn outcome.

### Native runtime observations

The continuous observer reads only the exact transcript directory bound by an
accepted native transition. It verifies the CURRENT pointer, manifest and
runtime-state digests, native versions, and session/parent identities. It does
not attach to a second app-server instance or choose the newest directory.

Child observations retain assigned names and parent relationships. Each native
child turn has its own stable execution span, so sending another message to a
finished child opens a new span. A reservation can fail before the first turn
starts. A closing child retains its running turn until Vibe settles or removes it.
When a turn disappears without a recorded outcome, its result is unknown. Persisted running,
completion, failure, interruption, and deletion states provide the available
evidence. A complete child run between samples can be recovered from its latest
outcome; several entirely completed runs between samples cannot all be
reconstructed from that one retained outcome. Unsupported/corrupt evidence and
capacity failures are reported instead of fabricating an empty child graph.

Vibe's session-start hook and persisted session pin name the active model
alias. Vibe maps that alias to a separately configured API model and provider,
and its routing or fallback can serve another model, so neither surface
establishes the model actually serving the session. Each native session start
therefore reports the root model as unavailable for that activation instead of
presenting the alias. Child models are not reported because Vibe keeps their
call-model information in the live app-server's memory rather than this store.

The execution directory is Vibe's actual default cwd captured at session start,
including a nested directory. It is not inferred from a repository root or
launch input. Later native hooks cannot reliably attribute cwd changes to a
source activation, so the add-on does not report mid-session worktree changes.

### Usage, context, and conversation export

Usage scans retain one cumulative record per native root, child, or fork
session. Repeated scans and exact resume update that stable record. Input and
output totals come from the verified public projection. Positive cache-read
amounts are lower bounds with unknown coverage; zero or missing cache data is
unknown, and cache creation is unavailable. A selected model cannot attribute
earlier accumulated tokens, so these records omit per-model attribution. Their
date allocation and overall historical coverage are explicitly incomplete.

Native context occupancy is separate from cumulative billed usage. The add-on
reports the last native context measurement and capacity, withdraws occupancy
when the journal is ahead or the replacement summary is unmeasured, and counts
only proven successful compaction checkpoints. Since repair/import can remove
history, lifetime compaction counts remain incomplete and are hidden by the
host rather than displayed as a complete total.

Exports use the retained full session ID, source locator, and frozen prepared
Vibe home. They preserve public user discussion, assistant text, tool arguments
and textual/structured results, and native outcome labels. Compact exports
retain user discussion, including prompts whose native turn was skipped, while
omitting known internal activity. Attachment references remain explicit; binary
attachments are not copied. Hidden startup/resume guidance is excluded by Vibe's
public history projection. A pending recovery journal or interleaved unsettled
history makes export unavailable until a sufficient native snapshot exists.

See [native usage, context, and export contracts](docs/native_usage_and_context.md)
for source locations, cache ownership, integrity checks, coverage boundaries,
and compaction-count authority.

### Owned implementation tasks

Logonomic can prepare a fresh Vibe implementation task through
`structured-session@3`, using the native streaming format qualified with Vibe 3.1.2. The host owns the attached
process, deadline, output, and cancellation. The provider passes the prompt on
stdin with native streaming output enabled, preserves the selected agent and
approval options, and uses the frozen launcher environment. The prompt is not
placed in command arguments. Preparation does not start the task.

Inspection binds the native user entry to the requested prompt after Vibe's
Unicode whitespace normalization, then retains that exact session and turn.
Success requires a complete native stream, a successful final process exit,
and committed assistant content from that task. An earlier assistant message
cannot establish success before the final exit facts arrive. Invalid output,
identity conflicts, unsuccessful exits, and timeouts remain failures.
The headless stream does not establish the task's effective model, so its
optional model report stays unavailable.

This capability declares only the implementation purpose. Read-only review is
unavailable because Vibe's `ask` approval mode does not enforce a read-only tool
policy. The provider does not add automatic approval. Saved-session selectors,
an existing launcher prompt, and explicit cwd overrides are refused for this
fresh-task route. Owned tasks retain the selected native profile. Vibe 3.1.2 closes each MCP
relay after listing its tools and opens another one for dispatch. Logonomic
admits one peer per task server, so delegated implementation remains unavailable.

See [owned task contracts](docs/native_owned_tasks.md) for native source,
inspection bounds, environment inputs, and qualification details.

### Remembered-session resume

The resume record retains the full native session ID, its absolute Vibe home,
and the native session directory. Preparation checks:

1. The current launch selects the original native home.
2. The native path has the expected `unified/<full-session-id>` layout.
3. `meta.json`, `CURRENT`, and the referenced generation manifest agree on the
   session and generation; the manifest's native byte digest matches `CURRENT`.
4. Required snapshot and recovery-journal files exist as regular files.
5. The saved native working directory matches the requested Logonomic workspace.
6. A supported Vibe executable and the expected managed hooks are available.

For this process only, the patch selects the recorded store with
`VIBE_SESSION_LOGGING__SAVE_DIR` and enables session logging with
`VIBE_SESSION_LOGGING__ENABLED=true`. Persistent Vibe settings are unchanged.
Supported agent, permission, and additional-directory arguments are preserved;
the bridge launches `vibe ... --resume <full-session-id>` with inherited stdin.

Missing, moved, corrupt, or unfamiliar store evidence is `unknown` during
inspection and prevents a prepared resume. This preserves remembered history
while avoiding known cases where native Vibe would retain a fresh startup
session after a failed resume. The adapter does not replay private journals or
prove that every conversation record is recoverable. Concurrent store changes
can still affect Vibe's own restore. Custom agent profiles or organization
policy that override `session_logging` outrank the resume environment. The
storage binding applies to ordinary user/project settings and the shipped
built-in agents.

## Build and package

The [contributor guide](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/CONTRIBUTING.md) covers local package checks.
Send provider pull requests to [the vibe repository](https://github.com/Varinomics/vnm_logonomic_cli_vibe/pulls).
See the [repository license](LICENSE) for the terms covering Varinomics code.

Required tools:

- A C++20 compiler, CMake, Git, and Ninja for the build below.
- Python 3.11+ for Vibe's contract tests.
- The SDK's [Linux packaging prerequisites](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#linux-native-runtime-deployment) when building on Linux.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/artifacts/vibe"
```

The single-configuration build produces
`build/package/logonomic.cli.vibe`. The export above produces
`artifacts/vibe/logonomic.cli.vibe`. See [SDK package export](https://github.com/Varinomics/vnm_logonomic_cli_addon/blob/master/sdk/logonomic_cli_addon_sdk/docs/package_contract.md#exporting-a-package) for the destination requirements.

With a multi-configuration generator, use `--config Release` for build/install,
`-C Release` for CTest, and find the package under
`build/package/Release/logonomic.cli.vibe`.

By default, configuration obtains the SDK and toml++ from their `master`
branches. Existing or pinned source checkouts can be supplied explicitly:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/vnm_logonomic_cli_addon/sdk/logonomic_cli_addon_sdk \
  -DVIBE_TOMLPLUSPLUS_SOURCE=/absolute/path/to/tomlplusplus
```

toml++ is used as a header-only TOML parser. Configuration verifies that its
selected license bytes match the shipped notice. The SDK assembles the native
package, deploys and records the Linux runtime closure, and writes its integrity
index. Standalone MSVC builds use the static C++ runtime. Embedded builds use
the SDK and runtime policy supplied by their parent project.

The SDK supports Linux x86_64, Windows x86_64, and macOS arm64 package targets.

## Tests and qualification

CTest groups cover launcher/version gates, TOML preservation and shell quoting,
native lifecycle and optional-capability classification, helper transport and
deadlines, correlated child observation, verified store/projection reads,
context and compaction evidence, cumulative usage/cache preservation, export
rendering, owned task preparation/inspection, packaged protocol behavior,
package validation, and SDK conformance.
Native contract fixtures cover unavailable and malformed evidence, replay,
pagination, counter regressions, source conflicts, provisional history,
structured tool results, role-correct discarded content, and cancellation.

The Linux qualification exercised the official Vibe **3.1.2** executable against the
upstream account-free localhost scripted model endpoint. It exercises real
native parent/child turns, hook delivery, persisted child observations, exact
interactive resume through the packaged bridge, root model availability and
source cwd reports, native context versus billing counters, stable usage
identities and selected cache reuse, and all three full/compact export formats. The exported discussion
excludes hidden model guidance. The scripted endpoint establishes native
transport and data behavior; it does not establish that a model obeys title or
summary instructions. No paid model backend is needed by this qualification.
Passing `--headless-resume` exercises the same saved conversation and bridge
through Vibe's native prompt mode. Both modes run in a custom selected profile.

The headless owned-task qualification runs on Windows and Linux with the same native version and local
endpoint for successful completion, an actual native model refusal, and a tool
approval denied by the native `ask` policy. In the last case, Vibe emits a
plausible answer and exits zero; the add-on still returns
`hook_approval_required`, and the denied command leaves no side effect. Actual
stdout is inspected in small fragments, with success withheld until final
process facts. A deliberately active inherited hook route is disabled for the
owned process, and the public prompt retains its exact normalized content.

Run `tests/vibe_native_qualification.py --help` or
`tests/vibe_native_tasks_qualification.py --help` for the installed executable,
matching upstream test-source checkout, built package, and fresh evidence
directory inputs. The package protocol fixture and real native CLI are separate
checks. The headless test uses an isolated profile and a loopback model endpoint.

Upstream source observation is recorded in
[`package/provenance/upstream_contract.json`](package/provenance/upstream_contract.json).
The minimum hook behavior was checked against the Vibe `v3.1.1` tag. A newer
semantic version can pass the version gate; its compatibility still depends on
the native hook and store contracts.

The [provider workflow](.github/workflows/ci.yml) declares its maintainer inputs.
