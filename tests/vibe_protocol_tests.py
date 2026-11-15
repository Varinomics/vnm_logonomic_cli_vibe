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
        transition = None if current_session is None else {
            "kind": "start", "provider_session_id": current_session, "nonce": "1" * 32,
        }
        return self.success("helper-ingress", 2, "classify_event", {
            "event": {"name": name, "body": body},
            "ledger": {"current_epoch": 0 if transition is None else 1, "current_transition": transition},
            "transition_nonce": "2" * 32,
        })

    def native_start(self, session, source="startup"):
        native_home = self.home / ".vibe"
        return {
            "session_id": session, "source": source,
            "vibe_home": str(native_home),
            "transcript_path": str(native_home / "logs" / "session" / "unified" / session),
        }

    def resume_fixture(self):
        executable, environment = self.installed_cli()
        session_id = "bc838a1c-14ec-4f08-8c0c-290d7f922c15"
        record = self.event("session_start", self.native_start(session_id))["observations"][0]["resume_record"]
        transcript = Path(record["transcript_path"])
        generation = "0000000000000001"
        generation_directory = transcript / "generations" / generation
        generation_directory.mkdir(parents=True)
        # These are native store identity witnesses, not a simulated transcript
        # or a claim that the adapter can replay Vibe's private journal.
        (transcript / "meta.json").write_text(json.dumps({"session_id": session_id}), encoding="utf-8")
        manifest = json.dumps({
            "manifest_version": 1, "session_id": session_id,
            "generation": generation, "snapshot_sequence": 0,
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
        for capability, version in (("launcher-discovery", 1), ("org.example.unknown", 1)):
            response = self.adapter.invoke(capability, version, "unknown_operation", {})
            self.assertEqual(response["status"], "unavailable", response)
            self.assertEqual(response["reason"], "unknown_operation", response)
        self.assertTrue(self.recognize("vibe")["recognized"])

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
