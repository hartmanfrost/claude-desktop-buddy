"""PlatformIO pre-build helper that injects FIRMWARE_VERSION into build flags.

Priority order:
  1. $FIRMWARE_VERSION environment variable (set by CI from the tag).
  2. `git describe --tags --always --dirty` output, stripped of leading 'v'.
  3. "0.0.0-dev" hardcoded fallback.

So:
  * Tag push v1.2.3 in CI → FIRMWARE_VERSION=1.2.3 (env wins).
  * Local hacking on a clean tag → 1.2.3 (git describe).
  * Local hacking with uncommitted changes → 1.2.3-N-gSHA-dirty.
  * No git history at all → 0.0.0-dev.

Hooked in via platformio.ini: `extra_scripts = pre:scripts/build/inject_version.py`.
"""
import os
import subprocess

Import("env")  # noqa: F821 — provided by PlatformIO


def _resolve_version() -> str:
    env_ver = os.environ.get("FIRMWARE_VERSION", "").strip().lstrip("v")
    if env_ver:
        return env_ver
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip().lstrip("v")
    except Exception:
        return "0.0.0-dev"


version = _resolve_version()
print(f"[inject_version] FIRMWARE_VERSION={version}")
env.Append(BUILD_FLAGS=[f'-DFIRMWARE_VERSION=\\"{version}\\"'])  # noqa: F821
