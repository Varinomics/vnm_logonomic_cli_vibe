# Native usage and context evidence

The provider reads the unified store written by Mistral Vibe 3.1.2. The source
oracle for this integration is upstream commit
[`b20313e01d6926d4191da89c32d5da11e3a0a060`](https://github.com/mistralai/mistral-vibe/tree/b20313e01d6926d4191da89c32d5da11e3a0a060).
Its public projection is distinct from both the private checkpoint and the
recovery journal. No conversation text is written to the provider's usage cache.

## Consistent native reads

`read_native_snapshot` checks the native session identity and the supported store
and runtime versions. `read_native_projection` then verifies that its immutable
generation still matches `CURRENT`, verifies the manifest and public projection
digests, and checks projection/session/sequence agreement. A history consumer
also verifies and hydrates each ordered content-addressed history chunk. A
counter-only reader does not load those message chunks.

The native journal can advance while the committed generation remains unchanged.
The reader reports that condition explicitly. It does not replay private Core
inputs, treat incomplete writes as valid, or claim the published generation is
the latest live state when journal records remain pending. Unknown layouts,
foreign identities, changed generations, links, corrupt digests, and exhausted
document bounds fail the affected read.

The writer and chunk/recovery formats are defined in
[`_storage.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_storage.py).

## Usage scanning

`usage-scan@3` reports one stable cumulative record for each native session.
Repeated scans replace that record; they do not append another copy of its
tokens. Native parent, child, and fork sessions retain separate identities.
`usage-session@1` maps those generated record keys back to their exact native
session IDs. Its `excludes_turn_in_flight` is false: committed calls can already
be counted while the surrounding turn is still running.

The scanner discovers these history locations from the frozen request:

- The default `VIBE_HOME/logs/session` directory.
- An explicit `VIBE_SESSION_LOGGING__SAVE_DIR` value.
- `session_logging.save_dir` in the user `VIBE_HOME/config.toml` file.

Each location contributes its `unified/<session_id>` directories. The same
native session found at multiple locations is counted once when its evidence
agrees; conflicting copies fail the scan, including a copy whose native counters
are still unavailable. This decision does not depend on directory ordering.
Other project, agent, and managed
configuration roots, legacy logs, deleted unobserved sessions, and unsaved
sessions are outside this discovery scope. The result therefore always declares
incomplete historical coverage. For a selected filesystem profile the scan,
temporal parent and accepted cache share the same source account key. Profiles
can reference a custom log directory. A stored session cannot establish which
remote principal was signed in when it ran.

The current accounting fields have these meanings:

| Field | Evidence and limits |
| --- | --- |
| Total, input, output | Native `snapshot.session.tokenUsage` cumulative counters; input plus output must equal total. |
| Cache read | A positive `cachedInputTokens` value is retained as a known lower bound with unknown coverage. Zero or absent evidence remains unknown. |
| Cache creation | Unknown: the public projection does not preserve the native cache-write counter. |
| Model | Omitted: the current selected alias cannot attribute earlier accumulated calls, and persisted child state does not contain actual call models. |
| Repository | Omitted: a working directory alone does not establish a repository root. |
| Temporal allocation | `usage-temporal@1` explicitly marks all lifetime tokens unavailable by date. The base compatibility date is the native snapshot's UTC update date. |
| Aggregate evidence | Partial: the surviving native counter is not proof of complete provider billing or lost historical evidence. |

Native completion parsing can normalize missing cache evidence to zero.
Consequently, zero in the projection cannot establish a measured zero cache
count. A positive sum also cannot establish that every contributing call
provided cache measurements. The model-call normalization is in
[`completion_profile/formats.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/completion_profile/formats.py).

The projector adds usage from committed assistant candidates and successful
compactions to the owning session. Failed or rejected calls are not a complete
billing source. Child tool results do not add the child's tokens into the parent
counter. Fork/import initialization copies conversation messages into a new
public session whose usage counters begin as unavailable; it does not copy the
source's lifetime usage. Resume retains the same native session key. These
boundaries are defined by
[`_projection.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_projection.py),
[`_host.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_host.py), and
[`_subagent_runs.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/_subagent_runs.py).

## Selected caches and pagination

`profile-storage@1` and the host's `cache_selection` own storage allocation and
acceptance. The provider validates the selected cache's exact inventory, digest,
format, and account scope before reading it. A corrupt, missing, or unknown
selected cache fails explicitly. There is no fallback to a guessed cache.

The next candidate starts with validated retained records, then merges current
native evidence. A disappeared session remains available from the selected
cache. A regressed native generation, sequence, total, input/output dimension,
or known cache-read lower bound rejects the candidate. A partial observation
cannot justify reducing previously accepted evidence. Explicit `reset` instead
starts from current sources with no selected cache.

The provider seals the complete candidate before returning its first page.
Every continuation is bound to the same frozen request, snapshot, scope,
candidate, and sorted records. It remains replayable after adapter restart or
native history changes. Pages contain at most 256 records and obey SDK transport
limits. Cancellation and failed scans preserve the selected cache. Only the host
reclaims cache directories.

## Context and compaction

`context-state@2` reads `snapshot.session.contextUsage`, independently of billed
tokens. Its current capacity comes from
`runtime.core_settings.context.model_facts.context_window`. Measurements reflect
Vibe's last native completion, not each streamed token.

A pending native journal, missing measurement, or native zero after compaction
clears old occupancy as unavailable. Vibe deliberately writes zero after a
successful compaction because the installed summary has not yet been measured;
the provider does not display that value as an empty conversation. Malformed
telemetry produces an independently session-scoped context failure and does not
invalidate lifecycle observations.

A fresh classifier request may supply `context_snapshot_revision`. Only that
request may receive an absolute successful-compaction count carrying the same
revision. The provider counts distinct completed `compaction` checkpoint IDs
with native success details, excludes entries containing errors, and never uses
the Core's attempt counter as successful evidence. Observe/poll readers do not
invent absolute-history sampling authority.

These counts are lower bounds. Native rewind, repair, and import can remove or
omit earlier successful-compaction entries, so the provider does not assert
complete lifetime compaction history. The SDK host retains that lower bound but
hides an incomplete count. The provider uses this single absolute-history
authority and does not also emit the same compactions as incremental events.

## Conversation export

The conversation-export capability uses version 2, requiring the VIBE_HOME
source fact prepared for the original launch or resume. An export requires the
exact retained provider session ID and resume locator from that source. The
locator's home must match the frozen environment; no current launcher account,
nearest session, abbreviated ID, or terminal scrollback substitutes for it.
Export does not depend on whether the CLI can resume the conversation, or on
whether its original working directory still exists.

The shared SDK writer renders the verified public projection as plain text,
Markdown, or highlighted conversation text. Native history/chunk order is
preserved. Identical duplicate entry IDs are suppressed; conflicting duplicates
fail. Text retains Unicode and formatting. Full exports distinguish tool
arguments, textual and structured tool results, native failures, reasoning, notices, checkpoints, and
answered user questions. Typed image/audio/blob attachments become explicit
markers; referenced files and URLs are not read or fetched. Embedded resource
text already present in the snapshot is retained.

Compact exports keep user discussion, including a prompt whose native turn was
skipped, question responses, and assistant text
whose final/commentary phase the source does not establish. They omit known
tool activity, reasoning, notices, approval records, and discarded provisional
assistant content. Full export labels discarded assistant text and reasoning
with their native cause. A skipped user prompt remains user discussion and is
labeled as skipped in both projections.

A purely unfinished history tail can be omitted with the SDK's explicit
incomplete-tail result flag. Interleaved completed discussion after unsettled
entries fails instead of being silently dropped. A pending recovery journal
also makes export temporarily unavailable: the published generation can lag
whole completed turns, so it cannot be passed off as a complete current export
or continuation source. Unknown entry/content kinds fail explicitly.

The writer publishes exactly one fixed-name artifact only after success, and
removes its own partial artifact after failure or cancellation. No native
history is modified. The provider's bounded native document reader reports
capacity failure explicitly; it does not truncate a successful export to an
IPC payload size.

## Remaining features and implementation routes

The interfaces below exist in the pinned native source. Their existence does
not mean this package has adopted them. The current interactive integration
owns native hooks and an authenticated store observer, not the interactive
client's app-server connection.

### Live session attachment

The native [app-server protocol](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/docs/reference/app-server-protocol.md)
opens a root through `session/start`, `session/resume`, or `session/continue`,
then supplies an atomic snapshot and attached event stream. One AppServer owns
one attached root and its children. The protocol explicitly does not promise
multiple simultaneous observers of the same Runtime. Starting another server
or resuming the same stored ID there does not attach to the already running CLI.

A live integration therefore needs a supported connection to that same Runtime,
or a launch route which owns and forwards the CLI's existing connection. It
must adopt native identity handoffs and watermarks, recover gaps with the native
snapshot protocol, and keep Logonomic activation attribution captured before
asynchronous delivery. The existing store observer cannot grant that control.

### Root outcomes and paired attention

The persisted public projection does contain `snapshot.latestTurn`, with a
native turn ID and completed, failed, or interrupted state. This is a useful
route for a bounded future observer of sampled terminal turns. It requires a
historical baseline, exact turn correlation, replay protection, and explicit
handling of pending journals and skipped, rejected, or limited turns. Native
`completed` alone does not prove successful completion of the user's task.
The reducer is in
[`_projection.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_projection.py).

That snapshot is not a complete root outcome log: a newer turn replaces the
latest turn, and generation publication can lag the journal. The optional
`session_api.turn_outcomes` ledger is recorded only when the native Session API
is enabled; ordinary sessions use `NoFacts`, as specified by
[`_fact_recorder.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_fact_recorder.py).
The qualified Vibe 3.1.2 CLI fixture has no such ledger. Complete live reporting
needs the attached turn/state stream and its gap-recovery rules. The existing
notification hooks omit native turn identity and suppress some terminal notices
while queued work or callbacks remain, so they remain inferred phase hints.

In a native 3.1.2 run, the first completion hook saw an immutable projection
whose turn was still in progress. After Resume, both the completion hook and
the exit hook saw the *previous* completed turn in that projection. At both
completion hooks, the pending journal contained a `projection_delta` with
the new turn's exact ID and completed status in `set_envelope.state.latestTurn`.
Reading these records requires the generation's sequence and digest chain.
[Native recovery journal](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_storage.py)

The host classifies a hook against the current session and activation. Its
ledger omits the admitted root turn ID. A delayed hook therefore cannot tie
a sampled journal outcome to the active turn, and Vibe emits no completion
notification for an interruption.
[Native notification construction](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/_host_hooks.py)

Native approval and question callbacks have stable IDs. The live protocol sends
`callback/call`; delivery acknowledgement is distinct from the semantic answer
submitted through `callback/result`. Answer, cancellation, and expiry must be
paired to that same native request. The Session overlays current
`activeCallbacks` on its live public state in
[`_session.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_session.py).
The recovery store retains pending registrations but drops settled callback
records during compaction in
[`_storage.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_storage.py).
Disappearance alone is not a matching resolution. A next paired-attention
implementation should consume the live callback state, reconcile reconnects,
and qualify approval, denial, answer, cancellation, and expiry before declaring
`paired_attention` supported.

### Ongoing model context and execution directory

Startup/resume guidance already uses native hidden `session_start` context.
Native `pre_agent` can also accept `additional_context`, but its
[`_foreign_hooks.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/harness/runtimes/python/python/mistralai_vibe_local_harness/vibe/_foreign_hooks.py)
always generates a visible added-context notice. Its payload also does not carry
the Logonomic activation nonce captured at session start. Looking up the host's
current nonce when a delayed hook arrives would not satisfy the SDK's
[`model-context-delivery@2`](https://github.com/Varinomics/logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/model_context_delivery_2.md)
contract. A future per-turn hook route needs both
source-captured attribution and a qualified native presentation path. On an
owned live connection, `session/context/inject` and state-appropriate
`turn/steer` provide separate native delivery routes; they still require exact
activation and acceptance handling.

Worktree movement does have a native fact after the tools move: the backend
emits `session/updated` with a `/cwd` patch from `_announce_cwd`. The earlier
worktree-prepared notification and the tool's returned target are insufficient;
the deferred move can still be dropped. See `enter_worktree`,
`_move_session`, and `_announce_cwd` in the
[`backend adapter`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/_unified_harness_backend_adapter.py).
The SDK's
[`execution-context@1`](https://github.com/Varinomics/logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/execution_context_1.md)
requires the actual default directory plus a
source-captured activation and sequence. Transcript metadata, per-command
directories, and launch input do not meet that contract. Until an attributed
live route is adopted, this package reports only its joint native session-start
cwd fact. Reporting a directory also does not independently prove a Git
worktree or repository identity. Current Logonomic hosts omit this optional
capability. Resume uses the conversation's registered directory.

### Authentication, quotas, and owned tasks

Native `providerAuth/read` is a redacted credential-source view. It deliberately
does not contact a provider, so a configured source is not authenticated proof;
see
[`_provider_auth.py`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/_provider_auth.py).
Native `account/read` can return ready, missing-key, unauthorized, or unavailable
state and a monthly Vibe Code budget with usage percentage and reset time.
These are real upstream sources for future
[`auth-status@2`](https://github.com/Varinomics/logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/auth_status_2.md)
and
[`quota@2`](https://github.com/Varinomics/logonomic/blob/master/sdk/logonomic_cli_addon_sdk/docs/quota_2.md)
probes. However, they are session-scoped APIs, and
[`AccountController.read`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/app_server/_account.py)
may reconcile tenant configuration after the lookup. A probe must establish
frozen provider/account scope, native credential custody, permitted effects,
timeouts, and redacted result mapping; it must not present another independently
opened session's account as the interactive session's current account. The
package independently resolves the local profile selected by the launcher's
native `VIBE_HOME` or platform default through `accounts@2`.

Fresh Logonomic-owned tasks are a separate route and do not need to observe the
interactive session. Native `-p` reads the prompt from stdin when no prompt
argument is supplied, and `--output streaming` emits completed public history
entries. The package's implementation-only `structured-session@3` route retains
that exact task session and turn, requires final stream completion and a
successful native exit, and returns committed assistant text. It preserves the
user's approval mode; `ask` is not a read-only policy. The native output contract
is in
[`headless_output.rs`](https://github.com/mistralai/mistral-vibe/blob/b20313e01d6926d4191da89c32d5da11e3a0a060/vibe/cli-rust/src/headless_output.rs).
See [owned task contracts](native_owned_tasks.md) for the adopted route and its
qualification. It does not need live attachment to the interactive session.
Vibe 3.1.2 uses separate stdio relay processes for tool discovery and dispatch.
Logonomic admits one peer per task server, so its delegated route is unavailable.
See the [relay lifetime qualification](native_owned_tasks.md#delegated-task-server-lifetime).
