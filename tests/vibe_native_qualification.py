#!/usr/bin/env python3
"""Opt-in POSIX qualification against an installed native Mistral Vibe.

Run with the Python environment that supplies Vibe's upstream test dependencies
(pytest, pexpect, pyte, tomli-w), an installed Vibe executable, and its matching
source checkout. No dependency is downloaded by this test. For example:

    python tests/vibe_native_qualification.py \
        --vibe /absolute/path/to/bin/vibe \
        --vibe-source /absolute/path/to/mistral-vibe \
        --package /absolute/path/to/logonomic.cli.vibe \
        --output-directory /absolute/path/to/new-evidence-directory

The maintained upstream scripted provider exercises real native model turns,
subagent creation, persistence, hooks, and interactive resume. All model requests
use a loopback endpoint and a dummy key; the home, workspace, and SDK host fixture
are test-owned. This qualifies the package boundary, not the Logonomic desktop.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


RESUME_PROMPT = "Continue the exact saved delegation conversation."
RESUME_ANSWER = "Resumed the exact saved conversation."
TITLE_GUIDANCE = "Logonomic guidance fixture: keep a meaningful task title and include a concise final summary."


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def success(adapter, capability, version, operation, payload):
    frame = adapter.invoke(capability, version, operation, payload)
    require(frame["status"] == "success", frame)
    return frame["payload"]


def supports(adapter, capability, version):
    return any(item["capability_id"] == capability and item["version"] == version
               for item in adapter.capabilities_frame["capabilities"])


def reserve_correlation(adapter, nonce):
    if supports(adapter, "lifecycle-correlation-lifetime", 1):
        success(adapter, "lifecycle-correlation-lifetime", 1, "reserve", {"transition_nonce": nonce})


def observe_one_change(adapter, session, workspace, nonce, resume_cursor=""):
    """Let the real SDK watch publish a page, then cancel its bounded window."""
    adapter.sequence += 1
    request_id = f"r{adapter.sequence}"
    adapter.send({
        "type": "invoke", "request_id": request_id,
        "capability_id": "lifecycle-events", "version": 2,
        "operation": "observe", "deadline_ms": 65000,
        "payload": {
            "session": {"working_directory": str(workspace), "launcher_argv": ["vibe"],
                        "provider_session_id": session},
            "resume_cursor": resume_cursor, "window_ms": 60000,
            "correlation": {"version": 1, "transition_nonce": nonce},
        },
    })
    cancelled = False
    for _ in range(32):
        frame = adapter.receive(timeout=10)
        require(frame.get("request_id") == request_id, frame)
        if frame["type"] == "progress" and not cancelled:
            adapter.send({"type": "cancel", "request_id": request_id})
            cancelled = True
        elif frame["type"] == "result":
            require(cancelled and frame["status"] == "success", frame)
            return frame["payload"]["resume_cursor"]
    raise AssertionError("Observer did not complete its cancelled window")


def qualify_native_observer(adapter, root, workspace, start_event, snapshots, child_turn_id):
    """Replay real native generations through the packaged watcher and SDK lane.

    Only these copied stores are changed. The actual Vibe conversation and its
    private app-server connection remain untouched.
    """
    if not supports(adapter, "lifecycle-correlation-lifetime", 1):
        return None
    session_id = start_event["body"]["session_id"]
    replay = root / "observer-replay" / "unified" / session_id
    shutil.copytree(snapshots["child-running"], replay)
    nonce = "3" * 32
    reserve_correlation(adapter, nonce)
    event = json.loads(json.dumps(start_event))
    event["body"]["transcript_path"] = str(replay)
    success(adapter, "helper-ingress", 2, "classify_event", {
        "event": event, "ledger": {"current_epoch": 0, "current_transition": None},
        "transition_nonce": nonce,
    })
    watch_cursor = observe_one_change(adapter, session_id, workspace, nonce)
    correlation = {"version": 1, "transition_nonce": nonce}
    first = success(adapter, "lifecycle-events", 2, "poll_observations", {
        "resume_cursor": "", "limit": 16, "correlation": correlation,
    })
    require(len(first["observations"]) == 1, first)
    started = first["observations"][0]
    require(started["subagent_transition"] == "started" and started["agent_id"] == child_turn_id, started)
    acknowledged = success(adapter, "lifecycle-events", 2, "poll_observations", {
        "resume_cursor": first["resume_cursor"], "limit": 16, "correlation": correlation,
    })
    require(not acknowledged["observations"], acknowledged)

    # Publishing the captured completed generation is the same immutable
    # CURRENT replacement the native writer made during the actual turn.
    shutil.copytree(snapshots["child-settled"], replay, dirs_exist_ok=True,
                    ignore=lambda _directory, names: ["CURRENT"] if "CURRENT" in names else [])
    replacement = replay / "CURRENT.next"
    replacement.write_bytes((snapshots["child-settled"] / "CURRENT").read_bytes())
    replacement.replace(replay / "CURRENT")
    observe_one_change(adapter, session_id, workspace, nonce, watch_cursor)
    query = {"resume_cursor": first["resume_cursor"], "limit": 16, "correlation": correlation}
    last = success(adapter, "lifecycle-events", 2, "poll_observations", query)
    repeated = success(adapter, "lifecycle-events", 2, "poll_observations", query)
    require(last == repeated, "Repeated observer page was not replayable before acknowledgement")
    require(len(last["observations"]) == 1, last)
    stopped = last["observations"][0]
    require(stopped["subagent_transition"] == "stopped" and stopped["agent_id"] == child_turn_id
            and stopped["provider_event_name"] == "vibe_subagent_idle", stopped)
    for observation in (started, stopped):
        require(observation["provider_session_id"] == session_id and observation["provider_turn_id"] == "",
                "Child activity was attributed to a root turn")
        require(all(observation[key] == "none" for key in (
            "session_transition", "phase_transition", "outcome_transition", "attention_transition")),
            "Child activity mutated the root lifecycle")
    empty = success(adapter, "lifecycle-events", 2, "poll_observations", {
        "resume_cursor": last["resume_cursor"], "limit": 16, "correlation": correlation,
    })
    require(not empty["observations"] and not empty["more"], empty)
    success(adapter, "lifecycle-correlation-lifetime", 1, "retire", {
        "transition_nonce": nonce, "disposition": "abandoned",
    })
    result = {"real_native_generation_replay": True, "child_run_id": child_turn_id,
              "observed_states": ["running", "idle"], "retry_and_acknowledgement": True}
    write_json(root / "observer-qualification.json", result)
    return result


def file_hashes(directory):
    return {
        str(path.relative_to(directory)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.rglob("*") if path.is_file()
    }


def capture_generation(session, destination):
    for attempt in range(3):
        target = destination / f"attempt-{attempt}" / "unified" / session.name
        try:
            pointer_bytes = (session / "CURRENT").read_bytes()
            pointer = json.loads(pointer_bytes)
            generation_path = Path("generations") / pointer["generation"]
            shutil.copytree(session / generation_path, target / generation_path)
            (target / "CURRENT").write_bytes(pointer_bytes)
            shutil.copy2(session / "meta.json", target / "meta.json")
            manifest = json.loads((target / generation_path / "manifest.json").read_text("utf-8"))
            journal = Path(manifest["recovery_journal_segment"]["path"])
            (target / journal).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(session / journal, target / journal)
            require(pointer_bytes == (session / "CURRENT").read_bytes(), "Generation moved while copying")
            final = destination / "unified"
            target.parent.rename(final)
            target.parent.parent.rmdir()
            return final / session.name
        except (OSError, AssertionError):
            shutil.rmtree(destination / f"attempt-{attempt}", ignore_errors=True)
    raise AssertionError(f"Could not capture a stable native generation at {session}")


def runtime_state(session):
    pointer = json.loads((session / "CURRENT").read_text("utf-8"))
    generation = session / "generations" / pointer["generation"]
    manifest_bytes = (generation / "manifest.json").read_bytes().rstrip(b"\n")
    require(hashlib.sha256(manifest_bytes).hexdigest() == pointer["manifest_sha256"],
            "Native CURRENT manifest digest disagrees")
    manifest = json.loads(manifest_bytes)
    descriptor = manifest["runtime_state"]
    runtime_bytes = (generation / descriptor["path"]).read_bytes().rstrip(b"\n")
    require(hashlib.sha256(runtime_bytes).hexdigest() == descriptor["sha256"],
            "Native runtime-state digest disagrees")
    return json.loads(runtime_bytes)


def public_state(session):
    pointer = json.loads((session / "CURRENT").read_text("utf-8"))
    generation = session / "generations" / pointer["generation"]
    manifest = json.loads((generation / "manifest.json").read_text("utf-8"))
    descriptor = manifest["projection_state"]
    encoded = (generation / descriptor["path"]).read_bytes().rstrip(b"\n")
    require(hashlib.sha256(encoded).hexdigest() == descriptor["sha256"],
            "Native public-projection digest disagrees")
    state = json.loads(encoded)["snapshot"]
    for digest in descriptor.get("chunks", []):
        chunk = (session / "chunks" / (digest + ".json")).read_bytes().rstrip(b"\n")
        require(hashlib.sha256(chunk).hexdigest() == digest, "Native history-chunk digest disagrees")
        state["history"]["entries"].extend(json.loads(chunk))
    return state


def qualify_native_accounting(adapter, preparation, record, stage, previous=None):
    session = Path(record["transcript_path"])
    before = file_hashes(Path(record["vibe_home"]))
    native = public_state(session)
    classification = success(adapter, "helper-ingress", 2, "classify_event", {
        "event": {"name": "pre_agent", "body": {"session_id": session.name}},
        "ledger": {"current_epoch": 1, "current_transition": {
            "kind": "start", "provider_session_id": session.name, "nonce": "2" * 32}},
        "transition_nonce": "4" * 32, "context_state_version": 2, "context_snapshot_revision": 41,
    })
    require("context_failure" not in classification, classification)
    updates = classification["context_updates"]
    require(len(updates) == 1 and updates[0]["provider_session_id"] == session.name and
            updates[0]["used_tokens"] == native["session"]["contextUsage"]["totalTokens"] and
            updates[0]["compaction_count_revision"] == 41 and
            not updates[0].get("compaction_count_complete", False),
            "Context carrier confused cumulative usage, source identity, or history authority")
    capacity = runtime_state(session)["core_settings"]["context"]["model_facts"]["context_window"]
    require(updates[0]["context_window"] == capacity, "Native model context capacity was lost")
    poll = success(adapter, "lifecycle-events", 2, "poll_observations", {
        "resume_cursor": "", "limit": 16,
        "correlation": {"version": 1, "transition_nonce": "2" * 32}, "context_state_version": 2,
    })
    require(poll["context_updates"][0]["used_tokens"] == updates[0]["used_tokens"] and
            "compaction_count_revision" not in poll["context_updates"][0],
            "Continuous context reader acquired absolute-history sampling authority")
    request = {key: value for key, value in preparation.items() if key != "title_instruction"}
    candidate = Path(request["profile_storage"]["root"]) / ("usage-" + stage)
    candidate.mkdir()
    request.update({"mode": "incremental", "snapshot_id": None, "cursor": None,
                    "cache_selection": {"version": 1, "accepted": previous, "candidate_root": str(candidate)}})
    usage = success(adapter, "usage-scan", 3, "scan", request)
    require(usage["complete"] and usage["incomplete_historical_coverage"] and
            usage["profile_storage"] == request["profile_storage"], usage)
    records = {item["session_key"]: item for item in usage["records"]}
    for path in session.parent.iterdir():
        if not path.is_dir() or not (path / "CURRENT").is_file():
            continue
        counts = public_state(path)["session"].get("tokenUsage")
        if counts is None:
            continue
        item = records[path.name]
        require(item["total_tokens"] == counts["totalTokens"] and
                item["input"]["value"] == counts["inputTokens"] and
                item["output"]["value"] == counts["outputTokens"] and
                item["semantics"] == "cumulative-final" and "model" not in item,
                "Usage scan changed native cumulative counters or attributed a selected model to billing")
        require(item["cache_read"]["coverage"]["support"] == "unknown" and
                item["cache_creation"]["coverage"]["support"] == "unknown",
                "Native normalized cache counters became invented complete cache measurements")
    identities = [{"record_id": item["record_id"], "session_key": item["session_key"]}
                  for item in usage["records"]]
    described = success(adapter, "usage-session", 1, "describe", {"records": identities})
    require(all(item["session"] == {"version": 1, "provider_session_id": item["session_key"],
                                   "excludes_turn_in_flight": False} for item in described["records"]),
            "Native usage aliases or completed-turn coverage were fabricated")
    require(file_hashes(Path(record["vibe_home"])) == before, "Context or usage read modified native history")
    write_json(candidate.parent / ("usage-" + stage + ".json"), usage)
    return {"context": updates[0], "usage": usage, "records": records}


def qualify_native_export(adapter, root, preparation, record):
    before = file_hashes(Path(record["vibe_home"]))
    structured_results = [json.dumps(entry["state"]["output"]["structured_content"],
                                     sort_keys=True, separators=(",", ":"))
                          for entry in public_state(Path(record["transcript_path"]))["history"]["entries"]
                          if entry["type"] == "effect" and
                          entry.get("state", {}).get("output", {}).get("structured_content") is not None]
    require(structured_results, "The native fixture did not exercise structured tool output")
    descriptors = []
    for format_name in ("text", "markdown", "conversation"):
        for compact in (False, True):
            output = root / ("conversation-" + format_name + ("-compact" if compact else "-full"))
            output.mkdir()
            result = success(adapter, "conversation-export", 2, "export", {
                "provider_session_id": record["provider_session_id"], "resume_record": record,
                "working_directory": preparation["working_directory"],
                "launch_environment": preparation["launch_environment"],
                "output_directory": str(output), "format": format_name, "compact": compact,
                "profile_storage": preparation["profile_storage"],
            })
            artifact = output / result["file_name"]
            text = artifact.read_text("utf-8")
            require(RESUME_PROMPT in text and RESUME_ANSWER in text and "Delegate exploration" in text,
                    "Export omitted accepted user discussion or the resumed answer")
            require(TITLE_GUIDANCE not in text, "Export exposed private startup/resume hook context")
            if not compact:
                require(all(result in text for result in structured_results),
                        "Full export dropped a native structured tool result")
            require(result["bytes"] == artifact.stat().st_size and result["message_count"] > 0 and
                    not result["incomplete_tail_omitted"] and result.get("compact", False) == compact and
                    result["profile_storage"] == preparation["profile_storage"],
                    "Export descriptor did not match the completed native history or artifact")
            descriptors.append(result)
    require(file_hashes(Path(record["vibe_home"])) == before, "Conversation export changed native history")
    return {"formats": ["text", "markdown", "conversation"], "compact_variants": True,
            "hidden_hook_context_excluded": True, "accepted_discussion_preserved": True,
            "structured_tool_results_preserved": True,
            "artifacts": descriptors}


def create_host_fixture(path, deliveries):
    # A fresh process handles each SDK delivery; separate files keep concurrent
    # child/root observations intact without a shared append protocol.
    path.write_text(
        f"#!{sys.executable}\n"
        "import json, pathlib, sys, time, uuid\n"
        f"root = pathlib.Path({str(deliveries)!r})\n"
        "if sys.argv[1:] == ['provider-event', 'deliver', '--args-stdin']:\n"
        "    delivery = json.load(sys.stdin)\n"
        "    target = root / (str(time.time_ns()) + '-' + uuid.uuid4().hex + '.json')\n"
        "    target.write_text(json.dumps(delivery), encoding='utf-8')\n"
        "    print(json.dumps({'accepted': True, 'epoch': 1}))\n"
        "elif len(sys.argv) == 4 and sys.argv[1:3] == ['provider-event', 'acknowledge']:\n"
        "    (root / (sys.argv[3] + '.ack')).write_text('acknowledged', encoding='utf-8')\n"
        "elif sys.argv[1:] == ['request-title-instruction', '--args-stdin']:\n"
        "    request = json.load(sys.stdin)\n"
        "    (root / (uuid.uuid4().hex + '.guidance')).write_text(json.dumps(request), encoding='utf-8')\n"
        "    activation = request.get('delivery', {}).get('activation', {})\n"
        "    events = [json.loads(p.read_text('utf-8')) for p in root.glob('*.json')]\n"
        "    accepted = any(e['event']['name'] == 'session_start' and e['event']['body']['session_id'] == activation.get('provider_session_id') and e['transition_nonce'] == activation.get('transition_nonce') for e in events)\n"
        f"    print(json.dumps({{'accepted': True, 'instruction_required': True, 'instruction': {TITLE_GUIDANCE!r}}} if accepted else {{'accepted': False, 'reason': 'stale_activation'}}))\n"
        "else:\n"
        "    sys.exit(2)\n",
        encoding="utf-8",
    )
    path.chmod(0o700)


def isolated_environment(home, native_home, vibe):
    environment = {
        "PATH": str(vibe.parent) + os.pathsep + os.defpath,
        "HOME": str(home), "USERPROFILE": str(home), "VIBE_HOME": str(native_home),
        "MISTRAL_API_KEY": "fake-key", "TERM": "xterm-256color", "LANG": "C.UTF-8",
        "VIBE_TEST_DISABLE_KEYRING": "1", "VIBE_TEST_DISABLE_AUTO_TITLE": "1",
        "VIBE_TEST_DISABLE_MODEL_PROBE": "1", "VIBE_INPUT_GRACE_PERIOD_MS": "50",
        "VIBE_TYPING_GRACE_PERIOD_MS": "50", "OTEL_SDK_DISABLED": "true",
        "HTTP_PROXY": "http://127.0.0.1:9", "HTTPS_PROXY": "http://127.0.0.1:9",
        "ALL_PROXY": "http://127.0.0.1:9", "NO_PROXY": "127.0.0.1,localhost",
        "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
    }
    if os.name == "nt":
        system = Path(os.environ["SystemRoot"]) / "System32"
        environment.update({
            "SystemRoot": os.environ["SystemRoot"], "COMSPEC": str(system / "cmd.exe"),
            "PATH": os.pathsep.join(map(str, [vibe.parent, system, system / "WindowsPowerShell" / "v1.0"])),
            "PATHEXT": ".COM;.EXE;.BAT;.CMD", "LOCALAPPDATA": str(home / "AppData" / "Local"),
            "APPDATA": str(home / "AppData" / "Roaming"), "TMP": str(home), "TEMP": str(home),
        })
    return environment


def run(arguments):
    require(os.name == "posix", "This native qualification uses a POSIX pseudo-terminal")
    root = arguments.output_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    home = root / "home"
    native_home = home / ".vibe"
    workspace = root / "workspace"
    profile = root / "profile"
    deliveries = root / "deliveries"
    for path in (home, workspace, profile, deliveries):
        path.mkdir()
    vibe = arguments.vibe.absolute()
    package = arguments.package.resolve()
    source = arguments.vibe_source.resolve()
    require(vibe.is_file(), f"Installed Vibe executable is missing: {vibe}")
    require((source / "tests/e2e/mock_server.py").is_file(), "Vibe source test fixtures are missing")

    # Only the test driver imports upstream fixtures. Native Vibe runs in the
    # empty test workspace without PYTHONPATH, using its installed distribution.
    sys.path.insert(0, str(source))
    from tests.e2e.mock_server import StreamingMockServer
    from tests.e2e.common import (
        TERMINAL_COLUMNS, TERMINAL_ROWS, send_ctrl_c_until_quit_confirmation,
        wait_for_main_screen, wait_for_rendered_text, write_e2e_config,
    )
    from tests.e2e.agent_loop_characterization.test_subagents import (
        PARENT_PROMPT, SUBAGENT_NAME, SUBAGENT_ANSWER,
        _is_parent_request, _spawn_subagent_factory, assistant_text_chunks,
    )
    sys.path.remove(str(source))
    import pexpect

    spec = importlib.util.spec_from_file_location(
        "vibe_protocol_harness", Path(__file__).with_name("vibe_protocol_tests.py"))
    require(spec is not None and spec.loader is not None, "Adapter protocol harness is unavailable")
    harness = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(harness)
    environment = isolated_environment(home, native_home, vibe)
    os.environ.clear()
    os.environ.update(environment)
    snapshots = {}
    root_session = None

    def respond(index, payload):
        nonlocal root_session
        if root_session is None:
            starts = [json.loads(path.read_text("utf-8"))["event"] for path in deliveries.glob("*.json")]
            starts = [event for event in starts if event["name"] == "session_start"]
            require(len(starts) == 1, "Native session_start did not reach the packaged helper")
            root_session = Path(starts[0]["body"]["transcript_path"])
        messages = payload.get("messages") or []
        if any(message.get("role") == "user" and
               str(message.get("content", "")).startswith(RESUME_PROMPT)
               for message in messages):
            return assistant_text_chunks(RESUME_ANSWER, created=130)
        label = "child-running" if not _is_parent_request(payload) else "child-settled"
        if (label == "child-running" or index > 0) and label not in snapshots:
            snapshots[label] = capture_generation(root_session, root / "snapshots" / label)
        return _spawn_subagent_factory(index, payload)

    server = StreamingMockServer(chunk_factory=respond)
    server.start()
    adapter = None
    child = None
    captured = io.StringIO()
    try:
        write_e2e_config(native_home, server.api_base, settings=["enable_subagents = true"])
        adapter = harness.Adapter(package, root)
        launcher_argv = [str(vibe), "--auto-approve", "--trust"]
        preparation = {
            "launcher": {
                "script_body": " ".join('"' + word.replace('"', '"""') + '"' for word in launcher_argv),
                "runner_key": "direct", "environment_text": "",
            },
            "launcher_argv": launcher_argv,
            "launch_environment": {"platform": harness.PLATFORM, "effective_environment": environment},
            "working_directory": str(workspace), "title_instruction": "Native Vibe qualification",
            "declared_assets": [], "profile_storage": {"version": 1, "root": str(profile)},
        }
        patch = success(adapter, "launch-preparation", 3, "prepare", preparation)
        require(patch["disposition"]["lifecycle_events"], patch)
        require(patch["apply_managed_configuration"], patch)
        transformed = success(adapter, "managed-configuration", 1, "transform", {
            "grant": "4" * 32, "present": False, "content_base64": "",
        })
        (native_home / "hooks.toml").write_bytes(base64.b64decode(transformed["content_base64"], validate=True))
        host = root / "test_vnmctl"
        create_host_fixture(host, deliveries)
        launch_environment = dict(environment)
        launch_environment.update(patch["environment"]["set"])
        launch_environment.update({
            "LOGONOMIC_VNMCTL_PATH": str(host), "LOGONOMIC_SESSION_CAPABILITY": "1" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.vibe",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": adapter.digest,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": adapter.nonce,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(package / "helpers"),
        })
        result = subprocess.run(
            [str(vibe), "-p", PARENT_PROMPT, "--auto-approve", "--max-turns", "6",
             "--trust", "--output", "json", "--output-dir", str(root / "export")],
            cwd=workspace, env=launch_environment, capture_output=True, text=True, timeout=60,
        )
        (root / "headless.stdout").write_text(result.stdout, encoding="utf-8")
        (root / "headless.stderr").write_text(result.stderr, encoding="utf-8")
        require(result.returncode == 0, f"Native headless turn exited {result.returncode}: {result.stderr}")
        require("child-running" in snapshots and "child-settled" in snapshots,
                "Native parent/child requests did not publish both observation points")
        running = runtime_state(snapshots["child-running"])["subagents"]["children"][SUBAGENT_NAME]
        settled = runtime_state(snapshots["child-settled"])["subagents"]["children"][SUBAGENT_NAME]
        require(running["state"]["type"] == "running", running)
        require(settled["state"]["type"] == "idle", settled)
        outcome = settled["state"]["last_outcome"]
        require(outcome["type"] == "completed" and outcome["final_answer"] == SUBAGENT_ANSWER, outcome)
        require(running["state"]["active"]["turn_id"] == outcome["turn_id"], "Child turn correlation changed")

        events = [json.loads(path.read_text("utf-8")) for path in sorted(deliveries.glob("*.json"))]
        starts = [value for value in events if value["event"]["name"] == "session_start"]
        require(len(starts) == 1, f"Expected one helper-delivered root start: {starts}")
        for delivery in events:
            require((deliveries / (delivery["idempotency_key"] + ".ack")).is_file(),
                    f"Delivery was not acknowledged: {delivery['event']['name']}")
        observer_result = qualify_native_observer(
            adapter, root, workspace, starts[0]["event"], snapshots, outcome["turn_id"])
        reserve_correlation(adapter, "2" * 32)
        classification = success(adapter, "helper-ingress", 2, "classify_event", {
            "event": starts[0]["event"],
            "ledger": {"current_epoch": 0, "current_transition": None},
            "transition_nonce": "2" * 32,
            "model_state": {"version": 1},
            "execution_context": {"version": 1},
        })
        # Native session_start names only the active model alias. The joint
        # root fact is therefore unavailable rather than a confirmed alias.
        models = classification["model_state"]["updates"]
        require(len(models) == 1 and models[0] == {
            "provider_session_id": starts[0]["event"]["body"]["session_id"],
            "transition_nonce": "2" * 32, "sequence": "0", "state": "unavailable",
            "agent_id": "", "provider": "", "model": "", "source": "vibe.session_start",
        }, "The native root model state lost its activation or confirmed an alias")
        directories = classification["execution_context"]["updates"]
        require(directories == [{
            "provider_session_id": starts[0]["event"]["body"]["session_id"],
            "transition_nonce": "2" * 32, "sequence": "0", "state": "confirmed",
            "environment": "local", "cwd": str(workspace), "source": "vibe.session_start.cwd",
        }], "The native session's actual cwd did not reach joint classification")
        require(TITLE_GUIDANCE in str(server.requests[0]["messages"]),
                "Native startup did not deliver host-provided guidance to the model")
        record = next(observation["resume_record"] for observation in classification["observations"]
                      if "resume_record" in observation)
        session = Path(record["transcript_path"])
        initial_accounting = qualify_native_accounting(adapter, preparation, record, "before-resume")
        before_inspection = file_hashes(native_home)
        resume_request = dict(preparation, resume={"version": 1, "record": record})
        inspection_request = {key: value for key, value in resume_request.items() if key != "title_instruction"}
        inspected = success(adapter, "interactive-resume", 3, "inspect_resume", inspection_request)
        require(inspected["availability"] == "available", inspected)
        prepared = success(adapter, "interactive-resume", 2, "prepare_resume", resume_request)
        require(prepared["process"]["argv"][-2:] == ["--resume", record["provider_session_id"]], prepared)
        require(file_hashes(native_home) == before_inspection, "Resume preflight modified native data")
        write_json(root / "resume-inspection.json", inspected)
        write_json(root / "resume-preparation.json", prepared)

        resume_environment = dict(launch_environment)
        resume_environment.update(prepared["environment"]["set"])
        process = prepared["process"]
        child = pexpect.spawn(
            str(package / process["executable"]), process["argv"], cwd=str(workspace),
            env=resume_environment, encoding="utf-8", timeout=30,
            dimensions=(TERMINAL_ROWS, TERMINAL_COLUMNS),
        )
        child.logfile_read = captured
        wait_for_main_screen(child, timeout=20)
        child.send(RESUME_PROMPT + "\r")
        wait_for_rendered_text(child, captured, needle=RESUME_ANSWER, timeout=20)
        send_ctrl_c_until_quit_confirmation(child, captured, timeout=5)
        child.expect(pexpect.EOF, timeout=10)
        child.close()
        require(child.exitstatus == 0, f"Interactive bridge exited {child.exitstatus}/{child.signalstatus}")
        latest = server.requests[-1]
        require(TITLE_GUIDANCE in str(latest["messages"]),
                "Native resume did not carry the host-provided model guidance")
        require(TITLE_GUIDANCE not in captured.getvalue(), "Model guidance leaked into the visible conversation")
        require(any(message.get("role") == "user" and PARENT_PROMPT in str(message.get("content"))
                    for message in latest["messages"]), "Native resume lost the earlier parent prompt")
        resumed = [json.loads(path.read_text("utf-8"))["event"] for path in deliveries.glob("*.json")]
        require(any(event["name"] == "session_start" and event["body"].get("source") == "resume"
                    and event["body"]["session_id"] == record["provider_session_id"] for event in resumed),
                "The package did not receive the exact resumed session identity")
        require(all(value == "Bearer fake-key" for value in server.authorizations),
                "A scripted model request did not use the test-owned dummy key")
        snapshots["resumed"] = capture_generation(session, root / "snapshots" / "resumed")
        accounting = qualify_native_accounting(adapter, preparation, record, "after-resume",
                                               initial_accounting["usage"]["cache_candidate"])
        require(set(accounting["records"]) == set(initial_accounting["records"]) and
                accounting["records"][session.name]["total_tokens"] >
                initial_accounting["records"][session.name]["total_tokens"],
                "Exact resume duplicated usage identity or lost the new native completion")
        exported = qualify_native_export(adapter, root, preparation, record)
        version = subprocess.run([str(vibe), "--version"], env=environment, cwd=workspace,
                                 capture_output=True, text=True, timeout=10)
        summary = {
            "native_version": version.stdout.strip(), "model": latest["model"],
            "root_session_id": record["provider_session_id"], "child_name": SUBAGENT_NAME,
            "child_turn_id": outcome["turn_id"], "child_states": ["running", "idle"],
            "model_requests": len(server.requests), "resume_availability": inspected["availability"],
            "interactive_exact_resume": True, "native_store": str(session),
            "subagent_observer": observer_result,
            "native_model_report": models[0],
            "native_execution_directory": directories[0],
            "model_context_transport": "verified; scripted model does not establish instruction compliance",
            "native_context": accounting["context"],
            "native_usage": {"session_totals": {key: item["total_tokens"]
                                                for key, item in accounting["records"].items()},
                             "stable_resume_identity": True, "selected_cache_reused": True,
                             "per_model_attribution": "unavailable", "cache_coverage": "unknown"},
            "native_export": exported,
        }
        write_json(root / "result.json", summary)
        print(json.dumps(summary, indent=2))
    finally:
        if child is not None and child.isalive():
            child.terminate(force=True)
        if child is not None and not child.closed:
            child.close()
        (root / "interactive.txt").write_text(captured.getvalue(), encoding="utf-8")
        write_json(root / "requests.json", server.requests)
        server.stop()
        if adapter is not None:
            adapter.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--vibe", type=Path, required=True)
    parser.add_argument("--vibe-source", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        run(arguments)
    except Exception as error:
        print(f"Native Vibe qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
