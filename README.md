# Mistral Vibe CLI add-on for Logonomic

Native Logonomic integration for an installed [Mistral Vibe](https://github.com/mistralai/mistral-vibe)
CLI, built with the public [Logonomic CLI add-on SDK](https://github.com/Varinomics/logonomic/tree/master/sdk/logonomic_cli_addon_sdk).

**Package:** `logonomic.cli.vibe` · **Version:** `0.1.0` · **Minimum Vibe:** `3.1.1`

The package discovers Vibe, prepares its native lifecycle hooks, reports session
identity and activity, and resumes a remembered native conversation by its full
session ID. It contains a C++20 adapter, a native hook helper, the SDK launch
bridge, capability sidecars, an integrity index, and dependency notices.

## Implemented capabilities

| Capability | Behavior |
| --- | --- |
| `presentation@1` | Mistral Vibe display name and an original package icon. |
| `launcher-discovery@1` | Finds an installed `vibe` on the supplied search path, with a `.local/bin` fallback, and recognizes supported literal interactive invocations. |
| `launch-preparation@3` | Probes the selected executable's version and proposes the environment and managed configuration required for hooks. |
| `managed-configuration@1` | Adds four owned hooks to `VIBE_HOME/hooks.toml` through the host's configuration transaction. |
| `lifecycle-events@2` | Native session start/resume/end, confirmed working status, and inferred completion/attention hints. |
| `helper-ingress@2` | Delivers bounded native hook events through the SDK helper client and checks the host's active session ledger. |
| `interactive-resume@2` | Prepares the original full session ID with its native home and storage root, preserving supported launcher options. |
| `interactive-resume@3` | Reads bounded native metadata and generation witnesses to inspect remembered-session availability without modifying history. |

Token accounting, conversation export, quotas, account management, delegated
tasks, coordinated turn delivery, paired approvals, and authoritative turn
outcomes are not advertised by this version. Vibe 3 uses a unified session
store; an older `messages.jsonl` layout is not a valid substitute for its current
accounting or transcript contract.

## Using the package

Install Vibe separately and make its executable available in the environment
Logonomic supplies to the launcher. An ordinary launcher is:

```sh
vibe
```

The adapter accepts literal interactive invocations with an initial prompt,
`--agent`, `--add-dir`, `--auto-approve`/`--yolo`, `--smart-approve`, and `--trust`.
Exact remembered-session resume replaces an old resume/continue selector and
removes the original startup prompt so it is not submitted a second time.

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
| `logonomic-vibe-session-start` | `session_start` | Capture startup, resume, clear, or fork identity and the native session path. |
| `logonomic-vibe-pre-agent` | `pre_agent`, `match = "main"` | Observe root turn start, including a root fork, without treating subagents as the foreground conversation. |
| `logonomic-vibe-notification` | `notification` | Observe completion/failure and requests for user attention. |
| `logonomic-vibe-session-end` | `session_end` | Observe termination of the current native session. |

Commands resolve the packaged helper through the launch-specific
`VNM_VIBE_HOOK` environment variable. They become no-ops when that variable is
absent. POSIX uses a quoted shell guard; Windows uses a fixed PowerShell encoded
command so the helper path is passed literally across native shell choices.
No package path is pinned in the persistent hooks file.

The hook helper forwards only the fields used for identity and lifecycle
classification. Prompt text, responses, tool output, and notification messages
are discarded. Input is bounded to 1 MiB, and delivery plus acknowledgment has
a shared 3.5-second deadline inside Vibe's 5-second hook timeout. Hooks are
observational: malformed input or unavailable host delivery exits quietly.

Vibe notifications are asynchronous and lack turn IDs. Completion/failure
notifications therefore produce an **inferred** return-to-input phase, and
approval/question notifications produce **inferred**, unpaired attention. An
event for a previous native session is ignored. Same-session notification
ordering and turn interruption are not fully observable in the current native
hook contract; a completion notification is not a confirmed turn outcome.

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

## Build and export

Required tools:

- A C++20 compiler, CMake 3.25+, Git, and Ninja for the commands below.
- Python 3.11+ for contract tests and package tooling.
- Read access to `Varinomics/logonomic`, which supplies the public SDK source.
- On Linux, the SDK runtime packaging tools: `patchelf`, `binutils`, and valid
  Debian/Ubuntu package metadata for the runtime libraries being shipped.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/artifacts/vibe-0.1.0"
```

The single-configuration build produces
`build/package/logonomic.cli.vibe`. The export above produces
`artifacts/vibe-0.1.0/logonomic.cli.vibe`. Choose an absent per-package export
destination: the SDK intentionally refuses to overwrite an existing one.
Application package ingestion owns installed-generation updates.

With a multi-configuration generator, use `--config Release` for build/install,
`-C Release` for CTest, and find the package under
`build/package/Release/logonomic.cli.vibe`.

By default, configuration obtains the SDK and toml++ from their `master`
branches. Existing or pinned source checkouts can be supplied explicitly:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/absolute/path/to/logonomic/sdk/logonomic_cli_addon_sdk \
  -DVIBE_TOMLPLUSPLUS_SOURCE=/absolute/path/to/tomlplusplus
```

toml++ is used as a header-only TOML parser. Configuration verifies that its
selected license bytes match the shipped notice. The SDK assembles the native
package, deploys and records the Linux runtime closure, and writes its integrity
index. Standalone MSVC builds use the static C++ runtime. Embedded builds use
the SDK and runtime policy supplied by their parent project.

The SDK supports Linux x86_64, Windows x86_64, and macOS arm64 package targets.
Actual Windows/macOS execution still requires the CI qualification described
below.

## Tests and qualification

Seven CTest groups cover launcher recognition/version gates, TOML preservation
and shell quoting, native lifecycle classification, helper transport and its
deadline, packaged adapter protocol behavior, package validation, and SDK
conformance. The protocol tests use a native executable fixture and exercise
discovery, preparation, managed hooks, session transitions, stale events, exact
resume, and refusal paths. Native store fixtures establish the metadata checks;
they are not complete Vibe conversations.

Linux qualification also exercised the official Vibe **3.1.2** wheel's real
`--version` and `--help` commands and the packaged adapter's launch preparation
against that installed executable. Generated hooks were checked against the
upstream native hook schema. An authenticated interactive model turn and a
full native conversation restore have not been exercised.

Upstream source observation is recorded in
[`package/provenance/upstream_contract.json`](package/provenance/upstream_contract.json).
The minimum hook behavior was checked against the Vibe `v3.1.1` tag. A newer
semantic version can pass the version gate; its compatibility still depends on
the native hook and store contracts.

### CI setup

[`Provider CI`](.github/workflows/ci.yml) calls the organization's reusable
`cli-addon-provider.yml` workflow with `provider: vibe`. The repository must
have access to the existing **`VARINOMICS_REPO_READ_TOKEN`** secret, including
read access to this repository and the private Logonomic SDK repository.

A checkout failure with `Input required and not supplied: token` means the
workflow did not receive that secret. Enable the secret for this repository,
then rerun the workflow. This failure occurs before compilation or tests.
