"""Exercise the native hook through the SDK helper client without a live host."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def run_hook(executable, payload, environment, *arguments, timeout=15, expected_context=None):
    data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
    result = subprocess.run(
        [str(executable), *arguments],
        input=data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        timeout=timeout,
        check=False,
    )
    assert result.returncode == 0, result.returncode
    if expected_context is None:
        assert result.stdout == b"", result.stdout
    else:
        assert json.loads(result.stdout) == {
            "hook_specific_output": {"additional_context": expected_context},
        }, result.stdout
    assert result.stderr == b"", result.stderr


def main():
    executable = Path(sys.argv[1]).resolve()
    fixture = Path(sys.argv[2]).resolve()
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith("LOGONOMIC_")
    }
    run_hook(executable, {"prompt": "ordinary Vibe outside Logonomic"}, environment)
    with tempfile.TemporaryDirectory(prefix="vibe hook ") as temporary:
        root = Path(temporary)
        captured = root / "wrapper_delivery.json"
        acknowledged = root / "wrapper_delivery.json.ack"
        guidance = root / "wrapper_delivery.json.guidance"
        wrapper = root / ("fake_vnmctl.exe" if os.name == "nt" else "fake_vnmctl")
        shutil.copy2(fixture, wrapper)
        home = root / "profile with spaces"
        home.mkdir()
        transcript = home / "logs" / "session" / "native-session"
        environment.update({
            "LOGONOMIC_VNMCTL_PATH": str(wrapper),
            "LOGONOMIC_SESSION_CAPABILITY": "1" * 64,
            "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.vibe",
            "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "2" * 64,
            "LOGONOMIC_PROVIDER_GENERATION_NONCE": "3" * 32,
            "LOGONOMIC_PROVIDER_HELPERS_DIR": str(root),
            "VIBE_HOME": str(home),
            "VIBE_TEST_CAPTURE": str(captured),
        })
        payload = {
            "hook_event_name": "session_start",
            "session_id": "bc838a1c-14ec-4f08-8c0c-290d7f922c15",
            "source": "fork",
            "parent_session_id": "8cbad950-736a-4e5a-82ac-6bf46451a0db",
            "transcript_path": str(transcript),
            "vibe_home": "/untrusted-payload-home",
            "prompt": "private model content " * 12000,
        }
        run_hook(executable, payload, environment)
        delivery = json.loads(captured.read_text("utf-8"))
        assert acknowledged.read_text("utf-8") == delivery["idempotency_key"]
        assert delivery["event"] == {
            "name": "session_start",
            "body": {
                "session_id": payload["session_id"],
                "source": "fork",
                "vibe_home": str(home.resolve()),
                "transcript_path": str(transcript),
            },
        }, delivery
        assert len(json.dumps(delivery)) < 4096

        # Context is captured for the source activation before delivery. The
        # native response contains only host-provided context, never a prompt
        # replacement, implementation details, or a visible system message.
        guidance.unlink()
        run_hook(executable, payload, dict(environment, VIBE_TEST_GUIDANCE="accept"),
                 expected_context='Host title guidance.\nPreserve "quoted" text.')
        delivery = json.loads(captured.read_text("utf-8"))
        requests = [json.loads(line) for line in guidance.read_text("utf-8").splitlines()]
        assert requests == [{
            "source": "vibe", "hook_event_name": "session_start", "changed_files_requested": False,
            "delivery": {"version": 2, "kind": "model-context", "activation": {
                "provider_session_id": payload["session_id"],
                "transition_nonce": delivery["transition_nonce"],
            }},
        }], requests
        for mode in ("refuse", "malformed", "stall"):
            guidance.unlink()
            run_hook(executable, payload, dict(environment, VIBE_TEST_GUIDANCE=mode), timeout=5)
            assert len(guidance.read_text("utf-8").splitlines()) == 1, "refused context retried without its fence"
        guidance.unlink()
        run_hook(executable, {"hook_event_name": "pre_agent", "session_id": payload["session_id"]},
                 dict(environment, VIBE_TEST_GUIDANCE="accept"))
        assert not guidance.exists(), "turn hook requested visible model context"

        # Invalid and unsupported events remain observational no-ops and never
        # invoke the host wrapper or reveal their input in stdout/stderr.
        original_delivery = captured.read_bytes()
        original_acknowledgment = acknowledged.read_bytes()
        for invalid in [b"{malformed", b"x" * (1024 * 1024 + 1), {"hook_event_name": "post_tool"}]:
            run_hook(executable, invalid, environment)
        run_hook(executable, payload, environment, "pre_agent")
        wrong_package = dict(environment, LOGONOMIC_PROVIDER_PACKAGE_ID="logonomic.cli.other")
        run_hook(executable, payload, wrong_package)
        assert captured.read_bytes() == original_delivery
        assert acknowledged.read_bytes() == original_acknowledgment

        # A failed host transport cannot become a failed native Vibe hook.
        acknowledged.unlink()
        run_hook(executable, payload, dict(environment, VIBE_TEST_WRAPPER_FAIL="1"))
        assert captured.is_file()
        assert not acknowledged.exists()
        assert not guidance.exists(), "rejected event requested model context"

        # Vibe's outer five-second hook deadline must not kill the helper while
        # it is waiting for the host. The fixture would return only after eight
        # seconds; the helper must cancel its SDK wrapper and exit quietly first.
        captured.unlink()
        run_hook(executable, payload, dict(environment, VIBE_TEST_WRAPPER_STALL="1"), timeout=5)
        assert captured.is_file(), "fixture did not exercise an in-flight host call"
        assert not acknowledged.exists(), "cancelled delivery was acknowledged"

    print("Vibe native helper transport passed")


if __name__ == "__main__":
    main()
