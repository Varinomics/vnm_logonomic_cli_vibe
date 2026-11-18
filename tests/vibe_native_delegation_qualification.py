"""Measure Vibe's native MCP relay lifetime using a packaged structured task and a loopback model.

Supply the installed vendor CLI, its matching upstream test sources, the built package and the
actual Logonomic task-server relay. The controlled local status service compares a one-peer
route with a repeatable route. The latter is a diagnostic control, not a product capability.
"""

import argparse
import base64
import contextlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import uuid

import vibe_native_tasks_qualification as owned


class StatusService:
    def __init__(self, one_peer):
        self.one_peer = one_peer
        self.requests = []
        self.calls = []
        self.error = None
        token = uuid.uuid4().hex
        if os.name == "nt":
            import _winapi
            self.endpoint = r"\\.\pipe\vibe-task-" + token
            # Both byte-mode pipe instances exist before Vibe requests its first.
            self.pipes = [_winapi.CreateNamedPipe(self.endpoint, 3, 0, 2, 65536, 65536, 5000, 0)
                          for _ in range(2)]
        else:
            self.endpoint = str(Path(tempfile.gettempdir()) / ("vibe-task-" + token))
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(self.endpoint)
            self.listener.listen(2)
        self.worker = threading.Thread(target=self.serve, daemon=True)
        self.worker.start()

    @contextlib.contextmanager
    def accept(self, index):
        if os.name == "nt":
            import _winapi
            import msvcrt
            handle = self.pipes[index]
            try:
                _winapi.ConnectNamedPipe(handle, False)
            except OSError as error:
                if error.winerror != 535:  # ERROR_PIPE_CONNECTED
                    raise
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
            self.pipes[index] = None  # Ownership moved to the file descriptor.
            with os.fdopen(descriptor, "r+b", buffering=0) as stream:
                yield stream
        else:
            with self.listener.accept()[0] as client, client.makefile("rwb", buffering=0) as stream:
                yield stream

    def serve(self):
        try:
            for index in range(2):
                with self.accept(index) as stream:
                    owned.require(json.loads(stream.readline()) == {"logonomic_task_connection": 1}, "Missing relay prelude")
                    self.requests.append([])
                    if self.one_peer and index:
                        return
                    self.exchange(stream, self.requests[-1])
                    if self.calls:
                        return
        except Exception as error:
            self.error = error
        finally:
            if os.name == "nt":
                import _winapi
                for handle in self.pipes:
                    if handle is not None:
                        _winapi.CloseHandle(handle)
            else:
                self.listener.close()
                Path(self.endpoint).unlink(missing_ok=True)

    def exchange(self, stream, methods):
        for line in stream:
            frame = json.loads(line)
            method = frame["method"]
            methods.append(method)
            if "id" not in frame:
                continue
            if method == "initialize":
                result = {"protocolVersion": frame["params"]["protocolVersion"], "capabilities": {"tools": {}},
                          "serverInfo": {"name": "controlled-status-service", "version": "1"}}
            elif method == "tools/list":
                result = {"tools": [{"name": "status", "description": "Read the current task status",
                                     "inputSchema": {"type": "object", "properties": {}}}]}
            elif method == "tools/call":
                self.calls.append(frame["params"]["name"])
                owned.require(self.calls == ["status"], self.calls)
                result = {"content": [{"type": "text", "text": "receipt-from-task-service"}]}
            else:
                owned.require(method == "ping", method)
                result = {}
            stream.write(json.dumps({"jsonrpc": "2.0", "id": frame["id"], "result": result}).encode() + b"\n")


def run_case(one_peer, root, adapter, native, harness, options, fixtures):
    StreamingMockServer, write_e2e_config, assistant_text_chunks, single_tool_call_chunks = fixtures
    directory = root / ("single-peer" if one_peer else "repeatable-control")
    directory.mkdir()
    workspace = directory / "workspace"
    profile = directory / "profile"
    home = directory / "home"
    for path in (workspace, profile, home):
        path.mkdir()
    env = native.isolated_environment(home, home / ".vibe", options.vibe)
    if options.relay_lib:
        env["PATH"] = str(options.relay_lib) + os.pathsep + env["PATH"]
    # Only the controlled status read is requested. Grant it in the test's
    # saved Launcher so that a tool approval cannot mask a transport failure.
    argv = [str(options.vibe), "--trust", "--auto-approve"]
    request = {
        "purpose": "implementation", "launcher_argv": argv,
        "launcher": {"script_body": " ".join('"' + arg.replace('"', '"""') + '"' for arg in argv),
                     "runner_key": "direct", "environment_text": ""},
        "launch_environment": {"platform": harness.PLATFORM, "effective_environment": env},
        "working_directory": str(workspace), "declared_assets": [], "profile_storage": {"version": 1, "root": str(profile)},
        "prompt_base64": base64.b64encode(owned.PROMPT.encode()).decode(),
    }
    prepared = owned.success(adapter, "prepare", request)
    service = StatusService(one_peer)
    descriptor = directory / "descriptor.json"
    owned.write_json(descriptor, {"version": 1, "task_id": "probe", "relay_program": str(options.relay),
                                 "servers": [{"server_id": "coordination", "endpoint": service.endpoint}]})
    settings = ["[[mcp_servers]]", 'name = "task_probe"', 'transport = "stdio"',
                "command = " + json.dumps([str(options.relay)]),
                "args = " + json.dumps(["--descriptor", str(descriptor), "--server", "coordination"])]

    def respond(index, _payload):
        if index == 0:
            return single_tool_call_chunks(call_id="discover", tool_name="search_tool_functions",
                arguments={"mode": "best_match", "query": "task_probe status"})
        if index == 1:
            return single_tool_call_chunks(call_id="dispatch", tool_name="run_typescript",
                arguments={"code": "async function main() { return await tools.mcp_task_probe.status({}); }"})
        return assistant_text_chunks(owned.ANSWER)

    server = StreamingMockServer(chunk_factory=respond)
    server.start()
    try:
        write_e2e_config(home / ".vibe", server.api_base, settings=settings)
        process = prepared["process"]
        owned.require(process["executable"] == {"launcher_program": True}, process)
        env.update(process["environment"])
        completed = subprocess.run([str(options.vibe), *process["argv"]], cwd=workspace, env=env,
            input=process["stdin_payload"].encode(), capture_output=True, timeout=90)
        (directory / "stdout.jsonl").write_bytes(completed.stdout)
        (directory / "stderr.txt").write_bytes(completed.stderr)
        service.worker.join(timeout=5)
        owned.require(not service.worker.is_alive() and service.error is None, f"Relay endpoint failed: {service.error}")
        owned.require(completed.returncode == 0 and len(server.requests) == 3, "Unexpected native termination or model sequence")
        owned.require("mcp_task_probe.status" in str(server.requests[1]["messages"]), "Native task tool was not discovered")
        listed = service.requests[0]
        owned.require("initialize" in listed and "tools/list" in listed and "tools/call" not in listed, listed)
        owned.require(len(service.requests) == 2, f"Native peer lifetime changed: {service.requests}")
        result = owned.inspect(adapter, prepared["inspection"]["protocol_state_base64_chunks"], completed.stdout,
                               completed.stderr, final=True, exit_code=completed.returncode)
        owned.require(owned.decoded(result["result_base64_chunks"]).decode() == owned.ANSWER, result)
        outcome = str(server.requests[-1]["messages"])
        if one_peer:
            owned.require(not service.calls and "mcp_transport_failed" in outcome and service.requests[1] == [],
                          "A one-peer grant unexpectedly dispatched a task call")
        else:
            owned.require(service.calls == ["status"] and "receipt-from-task-service" in outcome,
                          "Repeatable control did not deliver the original service response")
        return {"native_exit": completed.returncode, "model_requests": len(server.requests),
                "mcp_requests_by_relay_peer": service.requests, "actual_status_calls": len(service.calls),
                "native_status_transport_error": "mcp_transport_failed" in outcome}
    finally:
        server.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("vibe", "vibe-source", "package", "relay", "output-directory"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--relay-lib", type=Path, help="Directory containing the relay's Qt libraries on Windows")
    options = parser.parse_args()
    if os.name != "nt" and not sys.platform.startswith("linux"):
        parser.error("This endpoint fixture supports Windows named pipes and Linux Unix sockets.")
    options.vibe = options.vibe.absolute()
    options.package = options.package.resolve()
    options.relay = options.relay.resolve(strict=True)
    root = options.output_directory.resolve()
    root.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(options.vibe_source.resolve()))
    from tests.e2e.common import write_e2e_config
    from tests.e2e.mock_server import StreamingMockServer
    from tests.e2e.agent_loop_characterization.support import assistant_text_chunks, single_tool_call_chunks
    sys.path.pop(0)
    harness = owned.load_local("delegated_protocol_harness", "vibe_protocol_tests.py")
    native = owned.load_local("delegated_native_harness", "vibe_native_qualification.py")
    version_env = native.isolated_environment(root / "version-home", root / "version-native", options.vibe)
    version = subprocess.run([str(options.vibe), "--version"], check=True, env=version_env, cwd=root,
                             capture_output=True, text=True, timeout=10)
    fixtures = StreamingMockServer, write_e2e_config, assistant_text_chunks, single_tool_call_chunks
    adapter = harness.Adapter(options.package, root)
    try:
        result = {"native_version": version.stdout.strip(), "relay": str(options.relay),
                  "cases": [run_case(mode, root, adapter, native, harness, options, fixtures) for mode in (False, True)]}
        owned.write_json(root / "result.json", result)
        print(json.dumps(result, indent=2))
    finally:
        adapter.close()


if __name__ == "__main__":
    main()
