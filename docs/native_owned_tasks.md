# Owned native Vibe tasks

The provider implements the implementation purpose of structured-session@3 using
a fresh, host-owned Vibe process. The native interface is the ordinary headless
CLI, qualified against Mistral Vibe 3.1.2 and upstream commit
[b20313e01d6926d4191da89c32d5da11e3a0a060](https://github.com/mistralai/mistral-vibe/tree/b20313e01d6926d4191da89c32d5da11e3a0a060).
It neither attaches a second app-server to an interactive conversation nor
claims that this stream observes an existing terminal session.

## Launch and permissions

Preparation validates the saved launcher against the frozen host argv and
environment, probes the selected native program's version, and proposes the
launcher-program reference with --output streaming -p. The host owns its working
directory, process, descendants, stdin and teardown. The adapter never starts the
task during preparation.

The selected agent, additional directories, trust flag and explicit approval
options remain intact. The provider adds no automatic-approval flag and does
not replace the user's native configuration. A headless request that requires
human approval is denied by Vibe. The corresponding native approval-denial
record becomes hook_approval_required, even if the model subsequently writes
a plausible final answer and the native process exits zero.

Read-only review is not declared. Requests for that purpose receive
unavailable/purpose_unavailable: selecting the native ask agent changes approval
behavior, and the available CLI tool filters do not establish a complete
read-only policy across configured tools and extensions. Saved-session
selectors, an existing initial prompt, unsupported launcher options and
VIBE_CWD overrides also fail preparation. Tasks therefore start with a fresh
native identity in the host-selected workspace. A selected account must resolve
to the same native Vibe home in the frozen launch environment. Preparation
echoes the host's exact account and storage scope, and the native process uses
that home.

The sidecar declares native model, agent, tool-policy, MCP and connection
environment inputs. Explicit launcher-authored inputs present in the frozen
environment are also preserved. Pass-through values are requested by name;
credential values are not copied into the preparation reply. Normal native
configuration and authentication remain Vibe's responsibility.

The host can compose captured ambient variables and authored launcher
assignments as well as the proposal. For that reason, simply omitting
interactive routing names is insufficient. The proposal explicitly sets
VNM_VIBE_HOOK to an empty value and excludes VNM_VIBE_* and LOGONOMIC_*
pass-through names. This disables the provider's managed hook during the owned
task and prevents it from publishing a foreground transition or requesting
foreground title guidance. Unrelated user-owned hooks remain native
configuration. The structured route does not insert hidden guidance into the
submitted public prompt.

These behaviors follow the native
[programmatic-mode contract](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/docs/programmatic-mode.md)
and the provider's SDK
[structured-session@3 contract](https://github.com/Varinomics/logonomic/blob/5954beb1b31b8784d2baf9c1b8d72b4e5a846c3e/sdk/logonomic_cli_addon_sdk/docs/structured_session_3.md).

## Exact input and native output

The prompt is written to stdin and the pipe is closed. It is not placed in argv.
Native stdin handling trims leading and trailing Unicode White_Space; the
provider applies the same normalization before launch and records only its
SHA-256 digest in the initial continuation. Internal whitespace and UTF-8
content remain unchanged. Invalid UTF-8, NUL and an empty normalized prompt fail
preparation. See
[headless_prompt.rs](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/cli-rust/src/headless_prompt.rs).

The native streaming output is JSONL containing completed public history
entries. It has no authoritative final-result marker. Inspection binds the
first turn_start user entry to the exact normalized prompt and records its
native session, turn and input-entry IDs. All subsequent result evidence must
belong to that session and turn, and assistant output must name the same input
entry. A second user-start or steering entry is foreign task evidence.

Identical repeated history entries are idempotent; conflicting copies of an
entry ID fail inspection. Unknown entry kinds, incomplete generation records,
missing identity or outcome fields, and malformed JSON fail explicitly. A
candidate answer comes only from completed, committed assistant text. Discarded
drafts cannot become results. Another tool, callback or non-discarded reasoning
step invalidates a preceding intermediate answer until a new committed assistant
answer appears. Explicit native interrupt or steer causes fail inspection even
if contradictory committed text follows.

Native discarded/failure has a narrower meaning: it describes content that was
not retained, including provisional reasoning a hook removed beside a committed
answer. It is not by itself evidence that the turn failed. Settling such
discarded content does not erase an eligible committed answer; final process
facts still decide whether the native task completed. This distinction is
defined in the native
[public projector](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_projection.py).

The parser retains partial lines across inspections, including splits inside a
UTF-8 scalar. Continuation stores partial bytes in base64 and is bounded by the
SDK's continuation and aggregate frame limits. A native line and result are
bounded by the SDK's 1 MiB stream/result limit. Exhausted bounds produce an
explicit failure; history is not evicted to manufacture success. Stdout bytes
are consumed exactly once. Stderr is never substituted for an answer.

The native record schema and streaming behavior are defined in
[models.py](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/models.py)
and
[headless_output.rs](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/cli-rust/src/headless_output.rs).

## Completion and failure

An inspection remains incomplete while the process runs, even when it already
contains a plausible answer. Success requires all of the following:

- The exact submitted input, native session and native turn were bound.
- Every emitted line was valid and complete, ending in its native newline.
- The last eligible answer is nonempty committed assistant text.
- No native human-approval denial was observed.
- The host supplied final process facts, with no timeout and native exit zero.

Native exit one reports startup_failed. Native exit three reports
vibe_task_incomplete, covering the native budget, deadline, length, refusal
and termination class without guessing which reason occurred. Exit four
reports vibe_task_aborted; other nonzero or signal exits report
vibe_task_process_failed. A host timeout reports vibe_task_timed_out.
Malformed or truncated stdout fails regardless of its apparent answer.
Completion always returns an empty continuation and exactly one result or
failure, as required by the SDK.

Native completion is evidence that the native turn finished; it is not an
independent assessment of the quality or correctness of the model's work.
Native public history entries carry no model identity, so the stream does not
identify the effective model. An inspection that opts into model-state@1
receives explicit vibe_task_model_unavailable evidence rather than an alias
borrowed from the launcher or a separate interactive session.

Native outcome and child-wait handling are in
[headless/turn.rs](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/cli-rust/src/headless/turn.rs).

## Qualification and remaining control scope

The vibe_task_tests target covers frozen preparation, purpose refusal, exact
stdin, Unicode trimming, process ownership, permission preservation, hook
masking, arbitrary byte fragmentation, duplicate/conflicting entries, foreign
identity, discarded drafts and successful retries, native approval denial,
nonzero exits, timeouts, malformed/truncated output and continuation bounds.

The opt-in tests/vibe_native_tasks_qualification.py runs the packaged
prepare/inspect operations against installed native Vibe with the matching
upstream scripted provider. It uses a loopback endpoint and dummy key, performs
successful completion, native refusal and native approval-denial runs, and
submits actual native stdout in small fragments. A managed hook with an active
ambient routing variable is installed in the test-owned home so the explicit
empty override is exercised through native hook execution. Raw native streams,
process diagnostics, protocol replies and a compact result file are retained
under the caller's new evidence directory. This does not qualify the Logonomic
desktop UI or another operating system.

Delegated-session@3 remains undeclared. Its host relay and MCP connection
contract requires more than launching a headless model: every required task
server must be connected through the exact host-provided relay and descriptor,
with reliable readiness and dispatch evidence. Native MCP configuration may be
overridden by agent or managed settings, and the ordinary streaming history
does not establish that readiness. The independently useful owned structured
task path does not claim delegated control, live interactive coordination or
per-model billing.
