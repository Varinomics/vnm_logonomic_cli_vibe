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
agrees; conflicting copies fail the scan. Other project, agent, and managed
configuration roots, legacy logs, deleted unobserved sessions, and unsaved
sessions are outside this discovery scope. The result therefore always declares
incomplete historical coverage. It does not claim selected-account attribution.

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
