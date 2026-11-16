#!/usr/bin/env python3
"""Qualify packaged owned tasks with an installed native Vibe and local model.

Run using the Python environment that supplies the matching upstream test
fixtures, without downloading anything:

    python tests/vibe_native_tasks_qualification.py \
        --vibe /absolute/path/to/bin/vibe \
        --vibe-source /absolute/path/to/mistral-vibe \
        --package /absolute/path/to/logonomic.cli.vibe \
        --output-directory /absolute/path/to/new-evidence-directory

The package prepares every fresh native process and inspects its actual stream
and exit. The scripted provider listens on loopback and uses only a dummy key.
This is a provider/owned-process qualification, not a desktop UI test.
"""

from __future__ import annotations

import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys


PROMPT = "\u00a0\nImplement the owned task.\nPreserve π and 雪.\r\n\u3000"
ANSWER = "The owned task finished with π and 雪."


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def load_local(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    require(spec is not None and spec.loader is not None, "Local qualification fixture is unavailable")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def chunks(value):
    return [base64.b64encode(value[offset:offset + 65536]).decode("ascii")
            for offset in range(0, len(value), 65536)]


def decoded(values):
    return b"".join(base64.b64decode(value, validate=True) for value in values)


def success(adapter, operation, payload):
    frame = adapter.invoke("structured-session", 3, operation, payload)
    require(frame["status"] == "success", frame)
    return frame["payload"]


def inspect(adapter, state, stdout, stderr=b"", *, final=False, exit_code=0, timed_out=False):
    return success(adapter, "inspect", {
        "purpose": "implementation",
        "inspection": {
            "protocol_state_base64_chunks": state,
            "stdout_base64_chunks": chunks(stdout),
            "stderr_base64_chunks": chunks(stderr),
            "final": final,
            "timed_out": timed_out,
            "exit_code": exit_code,
        },
    })


def run_case(case, root, adapter, harness, native, vibe, source_fixtures):
    StreamingMockServer, write_e2e_config, assistant_text_chunks, single_tool_call_chunks = source_fixtures
    directory = root / case
    directory.mkdir()
    home = directory / "home"
    workspace = directory / "workspace"
    profile = directory / "profile"
    for path in (home, workspace, profile):
        path.mkdir()
    native_home = home / ".vibe"
    marker = directory / "foreground-hook-ran"
    poison_hook = directory / "foreground-hook"
    poison_hook.write_text("#!/bin/sh\nprintf called > " + shlex.quote(str(marker)) + "\n", encoding="utf-8")
    poison_hook.chmod(0o700)
    environment = native.isolated_environment(home, native_home, vibe)
    environment["VNM_VIBE_HOOK"] = str(poison_hook)

    def respond(index, _payload):
        if case == "approval-denied" and index == 0:
            return single_tool_call_chunks(
                call_id="owned-denied-tool", tool_name="bash",
                arguments={"command": "touch must-not-exist.txt"})
        if case == "refused":
            return [
                StreamingMockServer.build_chunk(
                    created=1, delta={"role": "assistant", "content": "This task was refused."},
                    finish_reason=None),
                StreamingMockServer.build_chunk(
                    created=2, delta={}, finish_reason="refusal",
                    usage={"prompt_tokens": 3, "completion_tokens": 4}),
            ]
        return assistant_text_chunks(ANSWER)

    server = StreamingMockServer(chunk_factory=respond)
    server.start()
    try:
        write_e2e_config(native_home, server.api_base)
        transformed = adapter.invoke("managed-configuration", 1, "transform", {
            "grant": "4" * 32, "present": False, "content_base64": "",
        })
        require(transformed["status"] == "success", transformed)
        (native_home / "hooks.toml").write_bytes(base64.b64decode(
            transformed["payload"]["content_base64"], validate=True))
        argv = [str(vibe), "--agent", "ask", "--trust"]
        request = {
            "purpose": "implementation",
            "launcher": {
                "script_body": " ".join('"' + word.replace('"', '"""') + '"' for word in argv),
                "runner_key": "direct", "environment_text": "",
            },
            "launcher_argv": argv,
            "launch_environment": {"platform": harness.PLATFORM, "effective_environment": environment},
            "working_directory": str(workspace), "declared_assets": [],
            "profile_storage": {"version": 1, "root": str(profile)},
            "prompt_base64": base64.b64encode(PROMPT.encode("utf-8")).decode("ascii"),
        }
        prepared = success(adapter, "prepare", request)
        process = prepared["process"]
        require(process["executable"] == {"launcher_program": True} and
                process["argv"] == ["--agent", "ask", "--trust", "--output", "streaming", "-p"] and
                process["stdin"] == "payload" and process["stdin_payload"] == PROMPT.strip() and
                process["start"] == "attached" and process["working_directory"] == "session_workspace",
                "Owned process changed launcher policy, input channel, or native workspace")
        require(prepared["profile_storage"] == request["profile_storage"] and
                prepared["inspection"]["incremental"] and
                not prepared["inspection"].get("keep_stdin_open", False), prepared)
        require(process["environment"] == {"VNM_VIBE_HOOK": ""} and
                not any(name.lower().startswith(("logonomic_", "vnm_vibe_"))
                        for name in process["environment_names"]),
                "Owned process could activate the foreground helper")
        require("fake-key" not in json.dumps(prepared), "Provider echoed a credential value")
        # The desktop composes captured ambient and authored launcher variables,
        # then provider assignments. Exercise the same override precedence with
        # a deliberately active foreground hook in the ambient fixture.
        launched_environment = dict(environment)
        launched_environment.update({name: environment[name] for name in process["environment_names"]
                                     if name in environment})
        launched_environment.update(process["environment"])
        completed = subprocess.run(
            [str(vibe), *process["argv"]], input=process["stdin_payload"].encode("utf-8"),
            cwd=workspace, env=launched_environment, capture_output=True, timeout=45,
        )
        (directory / "stdout.jsonl").write_bytes(completed.stdout)
        (directory / "stderr.txt").write_bytes(completed.stderr)
        write_json(directory / "preparation.json", prepared)
        write_json(directory / "model-requests.json", server.requests)
        require(not marker.exists(), "Native managed hook used inherited foreground routing")
        require(not (workspace / "must-not-exist.txt").exists(), "Native task bypassed configured tool approval")
        require(server.requests and all(value == "Bearer fake-key" for value in server.authorizations),
                "Native model did not use the local dummy-key fixture")
        require(all(str(request_body.get("model")) == "mock-model" for request_body in server.requests),
                "Native selected model changed")
        entries = [json.loads(line) for line in completed.stdout.splitlines()]
        starts = [entry for entry in entries if entry.get("source") == "turn_start"]
        require(len(starts) == 1 and starts[0]["content"] == [{"type": "text", "text": PROMPT.strip()}],
                "Submitted public prompt was replaced by hidden guidance or unrelated history")
        require(all(entry["sessionId"] == starts[0]["sessionId"] for entry in entries),
                "Owned stream unexpectedly mixed native session identities")

        state = prepared["inspection"]["protocol_state_base64_chunks"]
        trace = []
        # Deliberately split both JSON records and UTF-8 scalars. Every actual
        # native stdout byte is submitted exactly once before the final facts.
        for offset in range(0, len(completed.stdout), 73):
            result = inspect(adapter, state, completed.stdout[offset:offset + 73])
            trace.append(result)
            require(not result["complete"] and "result_base64_chunks" not in result and
                    "failure_reason" not in result,
                    "Native bytes authenticated a result before final process facts")
            state = result["protocol_state_base64_chunks"]
        result = inspect(adapter, state, b"", completed.stderr, final=True, exit_code=completed.returncode)
        trace.append(result)
        write_json(directory / "inspection-trace.json", trace)
        require(result["complete"] and result["protocol_state_base64_chunks"] == [], result)
        if case == "success":
            require(completed.returncode == 0 and decoded(result["result_base64_chunks"]).decode("utf-8") == ANSWER
                    and "failure_reason" not in result, result)
            # The identical native answer cannot override authoritative process
            # failure facts. These calls use the final accepted continuation.
            timed_out = inspect(adapter, state, b"", final=True, exit_code=0, timed_out=True)
            interrupted = inspect(adapter, state, b"", final=True, exit_code=-15)
            require(timed_out.get("failure_reason") == "vibe_task_timed_out" and
                    interrupted.get("failure_reason") == "vibe_task_process_failed",
                    "A native answer overrode deadline/interruption evidence")
            write_json(directory / "failure-facts.json", {"timed_out": timed_out, "interrupted": interrupted})
        elif case == "approval-denied":
            require(completed.returncode == 0 and result.get("failure_reason") == "hook_approval_required"
                    and "result_base64_chunks" not in result, result)
        else:
            require(completed.returncode == 3 and result.get("failure_reason") == "vibe_task_incomplete"
                    and "result_base64_chunks" not in result, result)
        return {
            "native_exit": completed.returncode,
            "provider_result": "success" if case == "success" else result["failure_reason"],
            "native_session_id": starts[0]["sessionId"], "native_turn_id": starts[0]["turnId"],
            "stream_records": len(entries), "model_requests": len(server.requests),
            "fragmented_inspections": len(trace) - 1,
            "foreground_hook_inactive": True, "native_permission_preserved": True,
            "public_prompt_exact": True,
        }
    finally:
        server.stop()


def run(arguments):
    require(os.name == "posix", "This native qualification uses POSIX hook fixtures")
    root = arguments.output_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    vibe = arguments.vibe.absolute()
    source = arguments.vibe_source.resolve()
    package = arguments.package.resolve()
    require(vibe.is_file(), "Installed native Vibe is missing")
    sys.path.insert(0, str(source))
    from tests.e2e.mock_server import StreamingMockServer
    from tests.e2e.common import write_e2e_config
    from tests.e2e.agent_loop_characterization.support import assistant_text_chunks, single_tool_call_chunks
    sys.path.pop(0)
    fixtures = StreamingMockServer, write_e2e_config, assistant_text_chunks, single_tool_call_chunks
    harness = load_local("owned_protocol_harness", "vibe_protocol_tests.py")
    native = load_local("owned_native_harness", "vibe_native_qualification.py")
    adapter = harness.Adapter(package, root)
    try:
        require(any(item == {"capability_id": "structured-session", "version": 3}
                    for item in adapter.capabilities_frame["capabilities"]),
                "Packaged adapter does not advertise structured-session@3")
        cases = {case: run_case(case, root, adapter, harness, native, vibe, fixtures)
                 for case in ("success", "refused", "approval-denied")}
        version_environment = native.isolated_environment(root / "version-home", root / "version-native", vibe)
        version = subprocess.run([str(vibe), "--version"], env=version_environment, cwd=root,
                                 capture_output=True, text=True, timeout=10)
        require(version.returncode == 0, "Native version probe failed")
        summary = {
            "native_version": version.stdout.strip(), "capability": "structured-session@3",
            "purpose": "implementation", "model_endpoint": "loopback-only dummy-key fixture",
            "cases": cases, "desktop_ui_qualification": False,
            "delegated_mcp_control": "not implemented",
        }
        write_json(root / "result.json", summary)
        print(json.dumps(summary, indent=2))
    finally:
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
        print(f"Native owned-task qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
