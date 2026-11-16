#!/usr/bin/env python3
"""Exercise the packaged adapter over its real stdin/stdout protocol.

No Vibe account, network request or model turn is used. The CLI fixture only
implements version/help probes; native hook payloads are explicit test inputs.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import tomllib
import unittest


PACKAGE: Path
FAKE_CLI: Path | None = None
PLATFORM = (
    "windows-x86_64" if sys.platform == "win32"
    else "macos-arm64" if sys.platform == "darwin"
    else "linux-x86_64"
)


class Adapter:
    """A small bounded host that observes the installed executable directly."""

    def __init__(self, package: Path, scratch: Path):
        self.package = package
        self.manifest = json.loads((package / "logonomic-cli-addon.json").read_text("utf-8"))
        adapter = self.manifest["adapter"]
        executable = adapter["path"]
        if isinstance(executable, dict):
            executable = executable[PLATFORM]
        self.digest = "a" * 64
        self.nonce = "b" * 32
        self.sequence = 0
        self.frames: queue.Queue[bytes | None] = queue.Queue()
        self.errors = bytearray()
        environment = {
            key: value for key, value in os.environ.items()
            if key.upper() in {
                "PATH", "SYSTEMROOT", "WINDIR", "COMSPEC", "PATHEXT",
                "HOME", "USERPROFILE", "TEMP", "TMP", "LANG", "LC_ALL",
            }
        }
        environment.update(adapter.get("environment", {}))
        environment.update({
            "LOGONOMIC_ADDON_PACKAGE_ID": self.manifest["package_id"],
            "LOGONOMIC_ADDON_GENERATION_DIGEST": self.digest,
            "LOGONOMIC_ADDON_GENERATION_NONCE": self.nonce,
            "LOGONOMIC_ADDON_PACKAGE_ROOT": str(package),
            "LOGONOMIC_ADDON_SCRATCH_DIR": str(scratch),
            "LOGONOMIC_ADDON_CORE_PROTOCOL_GENERATIONS": "1",
            "LOGONOMIC_ADDON_HOST_PRODUCT": "vibe-protocol-tests",
        })
        self.process = subprocess.Popen(
            [str(package / executable), *adapter.get("argv", [])],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=scratch, env=environment,
        )
        self.reader = threading.Thread(target=self._read_frames, daemon=True)
        self.error_reader = threading.Thread(target=self._read_errors, daemon=True)
        self.reader.start()
        self.error_reader.start()
        self.send({
            "type": "hello", "core_protocol_generations": [1],
            "package_id": self.manifest["package_id"],
            "generation_digest": self.digest, "generation_nonce": self.nonce,
            "limits": {
                "max_frame_bytes": 4194304, "max_json_depth": 32,
                "max_in_flight_requests": 64,
                "max_progress_frames_per_request": 9223372036854775807,
            },
            "host": {"product": "vibe-protocol-tests", "version": "1"},
        })
        try:
            self.hello = self.receive()
            self.capabilities_frame = self.receive()
            assert self.hello["type"] == "hello", self.hello
            assert self.hello["package_id"] == self.manifest["package_id"], self.hello
            assert self.hello["generation_digest"] == self.digest, self.hello
            assert self.hello["generation_nonce"] == self.nonce, self.hello
            assert self.hello["core_protocol_generation"] == 1, self.hello
            assert self.capabilities_frame["type"] == "capabilities", self.capabilities_frame
        except BaseException:
            try:
                self.close()
            except Exception:
                pass
            raise

    def _read_frames(self):
        assert self.process.stdout is not None
        try:
            for line in self.process.stdout:
                self.frames.put(line)
        finally:
            self.frames.put(None)

    def _read_errors(self):
        assert self.process.stderr is not None
        for chunk in iter(lambda: self.process.stderr.read(1024), b""):
            self.errors.extend(chunk)
            del self.errors[:-8192]

    def send(self, frame: dict):
        assert self.process.stdin is not None
        self.process.stdin.write(json.dumps(frame, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n")
        self.process.stdin.flush()

    def receive(self, timeout: float = 10) -> dict:
        try:
            line = self.frames.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError("Adapter did not answer within deadline: " + self.errors.decode("utf-8", "replace")) from None
        if line is None:
            raise AssertionError("Adapter exited before its result: " + self.errors.decode("utf-8", "replace"))
        assert line.endswith(b"\n") and not line.endswith(b"\r\n"), line
        result = json.loads(line)
        assert isinstance(result, dict), result
        return result

    def invoke(self, capability: str, version: int, operation: str, payload: dict) -> dict:
        self.sequence += 1
        request_id = f"r{self.sequence}"
        self.send({
            "type": "invoke", "request_id": request_id,
            "capability_id": capability, "version": version,
            "operation": operation, "payload": payload, "deadline_ms": 5000,
        })
        for _ in range(1024):
            frame = self.receive()
            assert frame.get("request_id") == request_id, frame
            if frame["type"] == "result":
                return frame
            assert frame["type"] == "progress", frame
        raise AssertionError("Unexpected progress flood")

    def close(self):
        if self.process.poll() is None:
            try:
                self.send({"type": "shutdown", "grace_ms": 3000})
                assert self.process.stdin is not None
                self.process.stdin.close()
                status = self.process.wait(timeout=5)
                assert status == 0, (status, self.errors.decode("utf-8", "replace"))
            finally:
                if self.process.poll() is None:
                    self.process.kill()
                    self.process.wait(timeout=5)
        self.reader.join(timeout=1)
        self.error_reader.join(timeout=1)
        for stream in (self.process.stdout, self.process.stderr):
            if stream is not None:
                stream.close()


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vibe-protocol-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.home = self.root / "home with spaces"
        self.home.mkdir()
        self.workspace = self.root / "workspace"
        self.workspace.mkdir()
        self.adapter = Adapter(PACKAGE, self.root)
        self.addCleanup(self.adapter.close)
        self.event_sequence = 0
        self.session_nonces = {}

    def success(self, capability, version, operation, payload):
        response = self.adapter.invoke(capability, version, operation, payload)
        self.assertEqual(response["status"], "success", response)
        return response["payload"]

    def recognize(self, script, runner="direct"):
        return self.success("launcher-discovery", 1, "recognize", {
            "launcher": {"script_body": script, "runner_key": runner, "environment_text": ""},
            "ambient_environment": {}, "platform": PLATFORM,
        })

    def installed_cli(self, version="3.1.2"):
        if FAKE_CLI is None:
            self.fail("--fake-cli is required for native process preparation tests")
        binary = self.root / "bin with spaces"
        binary.mkdir(exist_ok=True)
        executable = binary / ("vibe.exe" if os.name == "nt" else "vibe")
        shutil.copy2(FAKE_CLI, executable)
        (binary / "fake-version.txt").write_text(version + "\n", encoding="utf-8")
        self.native_home = self.home / ".vibe"
        self.native_home.mkdir(exist_ok=True)
        environment = {
            "PATH": str(binary), "HOME": str(self.home),
            "USERPROFILE": str(self.home), "VIBE_HOME": str(self.native_home),
        }
        if os.name == "nt":
            environment["PATHEXT"] = ".COM;.EXE;.BAT;.CMD"
            for key in ("SystemRoot", "WINDIR", "COMSPEC", "TEMP", "TMP"):
                if key in os.environ:
                    environment[key] = os.environ[key]
        return executable, environment

    def preparation_request(self, executable, environment):
        profile = self.root / "addon-profile"
        profile.mkdir(exist_ok=True)
        return {
            "launcher": {
                "script_body": f'"{executable}" --agent plan',
                "runner_key": "direct", "environment_text": "",
            },
            "launcher_argv": [str(executable), "--agent", "plan"],
            "launch_environment": {"platform": PLATFORM, "effective_environment": environment},
            "working_directory": str(self.workspace),
            "title_instruction": "Vibe integration test",
            "declared_assets": [],
            "profile_storage": {"version": 1, "root": str(profile)},
        }

    def event(self, name, body, current_session=None):
        self.event_sequence += 1
        nonce = f"{self.event_sequence:032x}"
        self.success("lifecycle-correlation-lifetime", 1, "reserve", {"transition_nonce": nonce})
        transition = None if current_session is None else {
            "kind": "start", "provider_session_id": current_session,
            "nonce": self.session_nonces.get(current_session, "1" * 32),
        }
        result = self.success("helper-ingress", 2, "classify_event", {
            "event": {"name": name, "body": body},
            "ledger": {"current_epoch": 0 if transition is None else 1, "current_transition": transition},
            "transition_nonce": nonce,
        })
        if result["classification"] == "transition":
            self.session_nonces[body["session_id"]] = nonce
        else:
            self.success("lifecycle-correlation-lifetime", 1, "retire", {
                "transition_nonce": nonce, "disposition": "abandoned",
            })
        return result

    def native_start(self, session, source="startup"):
        native_home = self.home / ".vibe"
        return {
            "session_id": session, "source": source,
            "vibe_home": str(native_home),
            "transcript_path": str(native_home / "logs" / "session" / "unified" / session),
        }

    def resume_fixture(self, save_directory=None):
        executable, environment = self.installed_cli()
        session_id = "bc838a1c-14ec-4f08-8c0c-290d7f922c15"
        started = self.native_start(session_id)
        if save_directory is not None:
            started["transcript_path"] = str(save_directory / "unified" / session_id)
        record = self.event("session_start", started)["observations"][0]["resume_record"]
        transcript = Path(record["transcript_path"])
        generation = "0000000000000001"
        generation_directory = transcript / "generations" / generation
        generation_directory.mkdir(parents=True)
        # These are native store identity witnesses, not a simulated transcript
        # or a claim that the adapter can replay Vibe's private journal.
        (transcript / "meta.json").write_text(json.dumps({
            "session_id": session_id,
            "environment": {"working_directory": str(self.workspace)},
        }), encoding="utf-8")
        descriptors = {}
        for key, filename in (("checkpoint", "checkpoint.json"),
                              ("runtime_state", "runtime-state.json"),
                              ("projection_state", "projection-state.json")):
            (generation_directory / filename).write_bytes(b"{}\n")
            descriptors[key] = {"path": filename, "sha256": hashlib.sha256(b"{}").hexdigest()}
        descriptors["checkpoint"]["checkpoint_version"] = 1
        (transcript / "journal").mkdir()
        (transcript / "journal" / "0000000000000001.jsonl").write_bytes(b"")
        manifest = json.dumps({
            "manifest_version": 1, "session_id": session_id,
            "generation": generation, "snapshot_sequence": 0,
            "created_at": "2026-01-01T00:00:00Z", "execution_state": "quiescent",
            **descriptors,
            "recovery_journal_segment": {"path": "journal/0000000000000001.jsonl", "first_sequence": 1},
        }).encode("utf-8")
        (generation_directory / "manifest.json").write_bytes(manifest + b"\n")
        (transcript / "CURRENT").write_text(json.dumps({
            "store_format": "mistral.vibe.unified-session-store/v1",
            "session_id": session_id, "generation": generation,
            "snapshot_sequence": 0, "manifest_sha256": hashlib.sha256(manifest).hexdigest(),
        }), encoding="utf-8")
        transformed = self.success("managed-configuration", 1, "transform", {
            "grant": "4" * 32, "present": False, "content_base64": "",
        })
        # Simulate only the host's successful managed-configuration commit.
        (self.native_home / "hooks.toml").write_bytes(base64.b64decode(transformed["content_base64"], validate=True))
        request = self.preparation_request(executable, environment)
        request["resume"] = {"version": 1, "record": record}
        return executable, request, record

    @staticmethod
    def snapshot(directory):
        return {str(path.relative_to(directory)): path.read_bytes()
                for path in directory.rglob("*") if path.is_file()}

    @staticmethod
    def canonical_digest(document):
        encoded = json.dumps(document, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def inspect_resume(self, preparation):
        request = {name: value for name, value in preparation.items() if name != "title_instruction"}
        result = self.success("interactive-resume", 3, "inspect_resume", request)
        self.assertEqual(result["request_digest"], self.canonical_digest(request))
        return result

    def test_handshake_matches_package_sidecars(self):
        advertised = {
            (item["capability_id"], item["version"])
            for item in self.adapter.capabilities_frame["capabilities"]
        }
        declared = set()
        for sidecar in (PACKAGE / "capabilities").glob("*.json"):
            content = json.loads(sidecar.read_text("utf-8"))
            declared.add((content["capability_id"], content["version"]))
        self.assertEqual(advertised, declared)
        self.assertIn(("launcher-discovery", 1), advertised)
        self.assertIn(("launch-preparation", 3), advertised)

    def test_unknown_operations_preserve_the_session(self):
        for capability, version in (("launcher-discovery", 1), ("structured-session", 3),
                                    ("org.example.unknown", 1)):
            response = self.adapter.invoke(capability, version, "unknown_operation", {})
            self.assertEqual(response["status"], "unavailable", response)
            self.assertEqual(response["reason"], "unknown_operation", response)
        self.assertTrue(self.recognize("vibe")["recognized"])

    def test_structured_implementation_admission(self):
        declaration = json.loads((PACKAGE / "capabilities" / "structured-session@3.json").read_text("utf-8"))
        self.assertEqual(declaration["payload"]["purposes"], ["implementation"])
        executable, environment = self.installed_cli()
        environment["MISTRAL_API_KEY"] = "dummy-structured-probe-key"
        request = self.preparation_request(executable, environment)
        request.pop("title_instruction")
        request["launcher"]["script_body"] = f'"{executable}" --agent ask'
        request["launcher_argv"] = [str(executable), "--agent", "ask"]
        prompt = 'Implement the requested change with "quotes" and λ.\nKeep $(literal) text.'
        request.update(purpose="read_only_review", prompt_base64=base64.b64encode(prompt.encode("utf-8")).decode("ascii"))

        refused = self.adapter.invoke("structured-session", 3, "prepare", request)
        self.assertEqual(refused["status"], "unavailable", refused)
        self.assertEqual(refused["reason"], "purpose_unavailable", refused)

        request["purpose"] = "implementation"
        result = self.success("structured-session", 3, "prepare", request)
        self.assertEqual(result["profile_storage"], request["profile_storage"])
        process = result["process"]
        self.assertEqual(process["executable"], {"launcher_program": True})
        self.assertEqual(process["argv"], ["--agent", "ask", "--output", "streaming", "-p"])
        self.assertEqual(process["working_directory"], "session_workspace")
        self.assertEqual(process["start"], "attached")
        self.assertEqual(process["stdin"], "payload")
        self.assertEqual(process["stdin_payload"], prompt)
        self.assertEqual(process["environment"]["VNM_VIBE_HOOK"], "")
        self.assertNotIn("MISTRAL_API_KEY", process["environment"])
        self.assertTrue(result["inspection"]["incremental"])
        self.assertFalse(result["inspection"]["keep_stdin_open"])
        state = b"".join(base64.b64decode(chunk, validate=True)
                         for chunk in result["inspection"]["protocol_state_base64_chunks"])
        self.assertNotIn(environment["MISTRAL_API_KEY"].encode(), state)
        self.assertNotIn(prompt.encode(), state)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_usage_storage_and_session_admission(self):
        executable, environment = self.installed_cli()
        request = self.preparation_request(executable, environment)
        request.pop("title_instruction")
        profile = request["profile_storage"]
        self.assertEqual(self.success("profile-storage", 1, "acknowledge", {
            "profile_storage": profile,
        }), {"profile_storage": profile})
        candidate = Path(profile["root"]) / "usage-candidate"
        candidate.mkdir()
        request.update({
            "mode": "incremental", "snapshot_id": None, "cursor": None,
            "cache_selection": {"version": 1, "accepted": None, "candidate_root": str(candidate)},
        })
        result = self.success("usage-scan", 3, "scan", request)
        self.assertEqual(result["profile_storage"], profile)
        self.assertEqual(result["records"], [])
        self.assertTrue(result["complete"])
        self.assertTrue(result["incomplete_historical_coverage"])
        self.assertEqual(result["cache_candidate"]["root"], str(candidate))
        self.assertTrue((candidate / "cache-manifest.json").is_file())
        session = "root-session"
        identity = {"record_id": "vibe-session:" + hashlib.sha256(session.encode()).hexdigest(),
                    "session_key": session}
        descriptions = self.success("usage-session", 1, "describe", {"records": [identity]})
        self.assertEqual(descriptions, {"records": [{**identity, "session": {
            "version": 1, "provider_session_id": session, "excludes_turn_in_flight": False,
        }}]})

    def test_observation_binding_requires_reserved_frozen_context(self):
        nonce = "e" * 32
        payload = {
            "event": {"name": "session_start", "body": self.native_start("root-session")},
            "ledger": {"current_epoch": 0, "current_transition": None}, "transition_nonce": nonce,
        }
        refused = self.adapter.invoke("helper-ingress", 2, "classify_event", payload)
        self.assertEqual(refused["status"], "unavailable", refused)
        self.assertEqual(refused["reason"], "vibe_correlation_not_reserved", refused)
        self.success("lifecycle-correlation-lifetime", 1, "reserve", {"transition_nonce": nonce})
        started = self.success("helper-ingress", 2, "classify_event", payload)
        self.assertEqual(started["classification"], "transition")
        self.success("lifecycle-correlation-lifetime", 1, "reserve", {"transition_nonce": nonce})
        payload["event"]["body"] = self.native_start("another-session")
        refused = self.adapter.invoke("helper-ingress", 2, "classify_event", payload)
        self.assertEqual(refused["status"], "unavailable", refused)
        self.assertEqual(refused["reason"], "vibe_correlation_context_mismatch", refused)
        polled = self.success("lifecycle-events", 2, "poll_observations", {
            "resume_cursor": "", "limit": 16, "correlation": {"version": 1, "transition_nonce": nonce},
        })
        self.assertEqual(polled["observations"], [])
        self.success("lifecycle-correlation-lifetime", 1, "retire", {
            "transition_nonce": nonce, "disposition": "abandoned",
        })
        refused = self.adapter.invoke("lifecycle-events", 2, "poll_observations", {
            "resume_cursor": polled["resume_cursor"], "limit": 16,
            "correlation": {"version": 1, "transition_nonce": nonce},
        })
        self.assertEqual(refused["status"], "unavailable", refused)
        self.assertEqual(refused["reason"], "vibe_correlation_unbound", refused)

    def test_recognition_does_not_claim_another_cli(self):
        self.assertTrue(self.recognize("vibe")["recognized"])
        self.assertTrue(self.recognize("vibe --agent plan")["recognized"])
        self.assertEqual(self.recognize("python"), {"recognized": False})
        self.assertEqual(self.recognize("vibe-other"), {"recognized": False})
        self.assertEqual(self.recognize("echo vibe", "bash"), {"recognized": False})

    def test_malformed_request_does_not_kill_adapter(self):
        response = self.adapter.invoke("launcher-discovery", 1, "recognize", {"launcher": 4})
        self.assertIn(response["status"], {"error", "unavailable"}, response)
        self.assertTrue(self.recognize("vibe")["recognized"])

    def test_discovery_uses_the_supplied_search_path(self):
        executable, environment = self.installed_cli()
        result = self.success("launcher-discovery", 1, "discover", {
            "ambient_environment": {name: value for name, value in environment.items()
                                    if name in {"PATH", "PATHEXT", "HOME", "USERPROFILE", "VIBE_HOME"}},
            "default_home_fallback": str(self.home), "platform": PLATFORM,
        })
        self.assertEqual(len(result["candidates"]), 1, result)
        candidate = result["candidates"][0]
        self.assertIn(str(executable), candidate["script_body"])
        self.assertTrue(candidate["default"])
        self.assertTrue(self.recognize(candidate["script_body"], candidate["runner_key"])["recognized"])

    def test_preparation_preserves_native_settings_until_host_transform(self):
        executable, environment = self.installed_cli()
        hooks = self.native_home / "hooks.toml"
        original = b'# user-owned comment\n[[hooks]]\nname="custom"\ntype="pre_agent"\ncommand="echo local"\n'
        hooks.write_bytes(original)
        request = self.preparation_request(executable, environment)
        result = self.success("launch-preparation", 3, "prepare", request)
        self.assertEqual(result["profile_storage"], request["profile_storage"])
        self.assertTrue(result["disposition"]["prepared"], result)
        self.assertTrue(result["disposition"]["lifecycle_events"], result)
        self.assertFalse(result["disposition"]["coordination_delivery"], result)
        self.assertTrue(result["apply_managed_configuration"], result)
        self.assertEqual(hooks.read_bytes(), original)
        hook_path = Path(result["environment"]["set"]["VNM_VIBE_HOOK"])
        self.assertEqual(hook_path.parent, PACKAGE / "helpers")
        self.assertTrue(hook_path.is_file())
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_old_vibe_does_not_receive_hooks_it_cannot_execute(self):
        executable, environment = self.installed_cli("3.1.0")
        result = self.adapter.invoke("launch-preparation", 3, "prepare",
                                     self.preparation_request(executable, environment))
        if result["status"] == "success":
            patch = result["payload"]
            self.assertFalse(patch["disposition"]["lifecycle_events"], patch)
            self.assertFalse(patch.get("apply_managed_configuration", False), patch)
        else:
            self.assertEqual(result["status"], "unavailable", result)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_initial_native_resume_selectors_remain_unmanaged(self):
        executable, environment = self.installed_cli()
        (self.native_home / "hooks.toml").write_text("# Native hooks stay unchanged.\n", encoding="utf-8")
        original = self.snapshot(self.native_home)
        request = self.preparation_request(executable, environment)
        for options in (["--resume"], ["--resume", "native-session"],
                        ["--resume=native-session"], ["--continue"], ["-c"]):
            with self.subTest(options=options):
                arguments = [str(executable), "--agent", "plan", *options]
                script = " ".join(f'"{argument}"' for argument in arguments)
                request["launcher"]["script_body"] = script
                request["launcher_argv"] = arguments
                self.assertTrue(self.recognize(script)["recognized"])
                result = self.success("launch-preparation", 3, "prepare", request)
                self.assertEqual(result["disposition"], {
                    "version": 1, "prepared": False, "lifecycle_events": False, "coordination_delivery": False,
                })
                self.assertEqual(result["environment"], {"set": {}, "unset": []})
                self.assertFalse(result.get("apply_managed_configuration", False))
                self.assertNotIn("argv", result)
                self.assertEqual(self.snapshot(self.native_home), original)
        # A positional prompt after -- is not a native saved-session selector.
        request["launcher_argv"] = [str(executable), "--", "--resume"]
        request["launcher"]["script_body"] = f'"{executable}" -- --resume'
        result = self.success("launch-preparation", 3, "prepare", request)
        self.assertTrue(result["disposition"]["prepared"])
        self.assertTrue(result["disposition"]["lifecycle_events"])
        self.assertTrue(result["apply_managed_configuration"])
        self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_managed_hooks_preserve_user_content_and_are_idempotent(self):
        original = '# My hooks stay intact.\n[[hooks]]\nname = "custom"\ntype = "pre_agent"\ncommand = "echo local"\n'
        grant = "3" * 32

        def transform(contents):
            result = self.success("managed-configuration", 1, "transform", {
                "grant": grant, "present": True,
                "content_base64": base64.b64encode(contents.encode("utf-8")).decode("ascii"),
            })
            self.assertEqual(result["grant"], grant)
            return base64.b64decode(result["content_base64"], validate=True).decode("utf-8")

        transformed = transform(original)
        self.assertIn(original, transformed)
        hooks = tomllib.loads(transformed)["hooks"]
        self.assertEqual(len(hooks), 5)
        self.assertEqual(hooks[0], {"name": "custom", "type": "pre_agent", "command": "echo local"})
        managed = hooks[1:]
        self.assertEqual({hook["type"] for hook in managed},
                         {"session_start", "pre_agent", "notification", "session_end"})
        self.assertEqual(next(hook for hook in managed if hook["type"] == "pre_agent")["match"], "main")
        self.assertEqual(transform(transformed), transformed)

    def test_native_clear_and_resume_replace_the_exact_identity(self):
        first_id = "bc838a1c-14ec-4f08-8c0c-290d7f922c15"
        second_id = "8cbad950-736a-4e5a-82ac-6bf46451a0db"
        first = self.event("session_start", self.native_start(first_id))
        self.assertEqual(first["classification"], "transition")
        self.assertEqual(first["transition"], {"kind": "start"})
        self.assertEqual(first["observations"][0]["provider_session_id"], first_id)
        self.assertEqual(first["observations"][0]["resume_record"]["provider_session_id"], first_id)
        cleared = self.event("session_start", self.native_start(second_id, "clear"), first_id)
        self.assertEqual(cleared["transition"], {"kind": "start"})
        self.assertEqual(cleared["observations"][0]["provider_session_id"], second_id)
        resumed = self.event("session_start", self.native_start(first_id, "resume"), second_id)
        self.assertEqual(resumed["transition"], {"kind": "resume"})
        self.assertEqual(resumed["observations"][0]["provider_session_id"], first_id)

    def test_lifecycle_hints_do_not_invent_confirmed_outcomes(self):
        session = "bc838a1c-14ec-4f08-8c0c-290d7f922c15"
        working = self.event("pre_agent", {"session_id": session, "turn_id": "native-turn"}, session)
        observation = working["observations"][0]
        self.assertEqual(observation["phase_transition"], "working")
        self.assertEqual(observation["phase_evidence"], "confirmed")
        self.assertEqual(observation["provider_turn_id"], "native-turn")
        for kind in ("turn_completed", "turn_failed"):
            with self.subTest(notification=kind):
                result = self.event("notification", {"session_id": session, "notification_type": kind}, session)
                observation = result["observations"][0]
                self.assertEqual(observation["phase_transition"], "awaiting_input")
                self.assertEqual(observation["phase_evidence"], "inferred")
                self.assertEqual(observation["outcome_transition"], "none")
                self.assertEqual(observation["outcome_evidence"], "none")
                self.assertEqual(observation["provider_turn_id"], "")
        for kind in ("approval_requested", "question_asked"):
            with self.subTest(notification=kind):
                result = self.event("notification", {"session_id": session, "notification_type": kind}, session)
                observation = result["observations"][0]
                self.assertEqual(observation["attention_transition"], "requested")
                self.assertEqual(observation["attention_evidence"], "inferred")
                self.assertEqual(observation["attention_request_id"], "")

    def test_stale_session_events_do_not_change_the_active_conversation(self):
        for name, extra in (("session_end", {"reason": "clear"}),
                            ("notification", {"notification_type": "turn_completed"}),
                            ("pre_agent", {"turn_id": "native-turn"})):
            with self.subTest(event=name):
                result = self.event(name, {"session_id": "old-conversation", **extra}, "current-conversation")
                self.assertEqual(result["classification"], "ignore")
                self.assertEqual(result["observations"], [])

    def test_exact_resume_uses_full_identity_and_preserves_launcher_options(self):
        executable, request, record = self.resume_fixture()
        original = self.snapshot(self.native_home)
        self.assertEqual(self.inspect_resume(request)["availability"], "available")
        for previous in (["--continue"], ["--resume", "outdated"], ["--resume=outdated"]):
            with self.subTest(previous=previous):
                preserved = ["--agent", "plan", "--auto-approve", "--add-dir", str(self.workspace)]
                arguments = [str(executable), *preserved, *previous, "old startup prompt"]
                request["launcher"]["script_body"] = " ".join(f'"{argument}"' for argument in arguments)
                request["launcher_argv"] = arguments
                result = self.success("interactive-resume", 2, "prepare_resume", request)
                process = result["process"]
                self.assertEqual(Path(process["executable"]).name,
                                 "vibe_launch_bridge.exe" if os.name == "nt" else "vibe_launch_bridge")
                self.assertEqual(process["argv"], [str(executable), *preserved, "--resume", record["provider_session_id"]])
                self.assertEqual(process["start"], "attached")
                self.assertEqual(process["stdin"], "inherit")
                self.assertEqual(process["working_directory"], "session_workspace")
                self.assertEqual(result["acknowledgment"], {
                    "version": 1, "record_digest": self.canonical_digest(record),
                })
                self.assertEqual(result["profile_storage"], request["profile_storage"])
                self.assertEqual(result["environment"]["set"]["VIBE_HOME"], record["vibe_home"])
        self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_restores_the_recorded_store_and_enables_session_logging(self):
        recorded_store = self.root / "original native session store"
        executable, request, record = self.resume_fixture(recorded_store)
        replacement_store = self.root / "replacement native session store"
        replacement_store.mkdir()
        request["launch_environment"]["effective_environment"].update({
            "VIBE_SESSION_LOGGING__SAVE_DIR": str(replacement_store),
            "VIBE_SESSION_LOGGING__ENABLED": "false",
        })
        (self.native_home / "config.toml").write_text("[session_logging]\nenabled = false\n", encoding="utf-8")
        original_home = self.snapshot(self.native_home)
        original_store = self.snapshot(recorded_store)
        self.assertEqual(self.inspect_resume(request)["availability"], "available")
        result = self.success("interactive-resume", 2, "prepare_resume", request)
        environment = result["environment"]["set"]
        self.assertEqual(environment["VIBE_SESSION_LOGGING__SAVE_DIR"], str(recorded_store))
        self.assertEqual(environment["VIBE_SESSION_LOGGING__ENABLED"], "true")
        self.assertEqual(environment["VIBE_HOME"], record["vibe_home"])
        self.assertEqual(result["process"]["argv"][-2:], ["--resume", record["provider_session_id"]])
        self.assertEqual(self.snapshot(self.native_home), original_home)
        self.assertEqual(self.snapshot(recorded_store), original_store)
        self.assertEqual(self.snapshot(replacement_store), {})
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_rejects_malformed_transcript_layout(self):
        executable, request, record = self.resume_fixture()
        transcript = Path(record["transcript_path"])
        original = self.snapshot(self.native_home)
        for path in (transcript.with_name("different-session"),
                     transcript.parent.parent / record["provider_session_id"]):
            with self.subTest(transcript=path):
                request["resume"]["record"] = {**record, "transcript_path": str(path)}
                self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
                result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
                self.assertEqual(result["status"], "error", result)
                self.assertEqual(result["reason"], "resume_record_invalid", result)
                self.assertNotIn("payload", result)
                self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_refuses_changed_or_missing_working_directory(self):
        executable, request, record = self.resume_fixture()
        metadata_path = Path(record["transcript_path"]) / "meta.json"
        metadata = json.loads(metadata_path.read_text("utf-8"))
        another_directory = self.root / "different workspace"
        another_directory.mkdir()

        def refused():
            original = self.snapshot(self.native_home)
            self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
            result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
            self.assertEqual(result["status"], "unavailable", result)
            self.assertEqual(result["reason"], "vibe_resume_directory_mismatch", result)
            self.assertNotIn("payload", result)
            self.assertEqual(self.snapshot(self.native_home), original)

        with self.subTest(directory="different launch workspace"):
            request["working_directory"] = str(another_directory)
            refused()
        request["working_directory"] = str(self.workspace)
        for replacement in ({"session_id": record["provider_session_id"]},
                            {"session_id": record["provider_session_id"], "environment": {}},
                            {"session_id": record["provider_session_id"],
                             "environment": {"working_directory": "relative/workspace"}}):
            with self.subTest(metadata=replacement):
                metadata_path.write_text(json.dumps(replacement), encoding="utf-8")
                refused()
        with self.subTest(directory="deleted native workspace"):
            metadata_path.write_text(json.dumps(metadata), encoding="utf-8")
            self.workspace.rmdir()
            refused()
        self.assertEqual(self.snapshot(another_directory), {})
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_missing_runtime_files_stays_unknown(self):
        executable, request, record = self.resume_fixture()
        transcript = Path(record["transcript_path"])
        generation = transcript / "generations" / "0000000000000001"
        required = [generation / "checkpoint.json", generation / "runtime-state.json",
                    generation / "projection-state.json", transcript / "journal" / "0000000000000001.jsonl"]
        for path in required:
            with self.subTest(missing=path.name):
                content = path.read_bytes()
                path.unlink()
                try:
                    original = self.snapshot(self.native_home)
                    self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
                    result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
                    self.assertEqual(result["status"], "unavailable", result)
                    self.assertEqual(result["reason"], "vibe_native_session_unavailable", result)
                    self.assertNotIn("payload", result)
                    self.assertEqual(self.snapshot(self.native_home), original)
                finally:
                    path.write_bytes(content)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_rejects_malformed_runtime_descriptors(self):
        executable, request, record = self.resume_fixture()
        transcript = Path(record["transcript_path"])
        current_path = transcript / "CURRENT"
        pointer = json.loads(current_path.read_text("utf-8"))
        manifest_path = transcript / "generations" / pointer["generation"] / "manifest.json"
        manifest = json.loads(manifest_path.read_text("utf-8"))
        for key, replacement in (
            ("checkpoint", None),
            ("runtime_state", {"path": "../runtime-state.json"}),
            ("projection_state", {"path": "nested/projection-state.json"}),
            ("recovery_journal_segment", {"path": "journal/../0000000000000001.jsonl", "first_sequence": 1}),
            ("recovery_journal_segment", {"path": "journal/0000000000000001.jsonl", "first_sequence": 2}),
        ):
            with self.subTest(descriptor=key, replacement=replacement):
                changed = {**manifest, key: replacement}
                if replacement is None:
                    del changed[key]
                body = json.dumps(changed).encode("utf-8")
                manifest_path.write_bytes(body + b"\n")
                # Keep the outer identity witness valid to exercise descriptor
                # rejection independently of the manifest checksum gate.
                pointer["manifest_sha256"] = hashlib.sha256(body).hexdigest()
                current_path.write_text(json.dumps(pointer), encoding="utf-8")
                original = self.snapshot(self.native_home)
                self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
                result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
                self.assertEqual(result["status"], "unavailable", result)
                self.assertEqual(result["reason"], "vibe_native_session_unavailable", result)
                self.assertNotIn("payload", result)
                self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_missing_or_changed_native_store_does_not_launch_a_fresh_session(self):
        executable, request, record = self.resume_fixture()
        current = Path(record["transcript_path"]) / "CURRENT"
        for replacement in (None, b"not json", json.dumps({
            "store_format": "mistral.vibe.unified-session-store/v1",
            "session_id": "another-native-session", "generation": "0000000000000001",
        }).encode("utf-8")):
            with self.subTest(pointer=replacement):
                if replacement is None:
                    current.unlink()
                else:
                    current.write_bytes(replacement)
                original = self.snapshot(self.native_home)
                self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
                result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
                self.assertEqual(result["status"], "unavailable", result)
                self.assertNotIn("payload", result)
                self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_stays_bound_to_its_original_native_home(self):
        executable, request, _ = self.resume_fixture()
        another_home = self.root / "another-vibe-profile"
        another_home.mkdir()
        request["launch_environment"]["effective_environment"]["VIBE_HOME"] = str(another_home)
        original = self.snapshot(self.native_home)
        self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
        result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
        self.assertEqual(result["status"], "unavailable", result)
        self.assertEqual(result["reason"], "vibe_resume_home_mismatch", result)
        self.assertEqual(self.snapshot(self.native_home), original)
        self.assertEqual(self.snapshot(another_home), {})
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_corrupt_generation_manifest_is_not_resumable(self):
        executable, request, record = self.resume_fixture()
        transcript = Path(record["transcript_path"])
        pointer = json.loads((transcript / "CURRENT").read_text("utf-8"))
        manifest = transcript / "generations" / pointer["generation"] / "manifest.json"
        manifest.write_bytes(manifest.read_bytes().replace(b'"manifest_version": 1', b'"manifest_version": 2'))
        original = self.snapshot(self.native_home)
        self.assertEqual(self.inspect_resume(request)["availability"], "unknown")
        result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
        self.assertEqual(result["status"], "unavailable", result)
        self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())

    def test_resume_requires_installed_logonomic_hooks(self):
        executable, request, _ = self.resume_fixture()
        hooks = self.native_home / "hooks.toml"
        hooks.write_text("# Only user configuration remains.\n", encoding="utf-8")
        original = self.snapshot(self.native_home)
        self.assertEqual(self.inspect_resume(request)["availability"], "available")
        result = self.adapter.invoke("interactive-resume", 2, "prepare_resume", request)
        self.assertEqual(result["status"], "unavailable", result)
        self.assertEqual(result["reason"], "vibe_resume_hooks_unavailable", result)
        self.assertEqual(self.snapshot(self.native_home), original)
        self.assertFalse((executable.parent / "unexpected-launch.txt").exists())


def main():
    global PACKAGE, FAKE_CLI
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--fake-cli", type=Path)
    arguments, remaining = parser.parse_known_args()
    PACKAGE = arguments.package.resolve(strict=True)
    FAKE_CLI = arguments.fake_cli.resolve(strict=True) if arguments.fake_cli else None
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)


if __name__ == "__main__":
    main()
