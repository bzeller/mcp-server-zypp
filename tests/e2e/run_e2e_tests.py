#!/usr/bin/env python3
"""
Host-side launcher for all mcp-server-zypp e2e scenarios.

Spins up a single throwaway podman container, builds zypp-mcp-tool fresh
against that container's own libzypp-devel package (see
container_test.py / e2e_common.build_worker()), then runs every scenario
module under scenarios/ inside that same container — one container, one
build, regardless of how many scenarios exist. Adding a new scenario
means adding a new scenarios/<name>.py file, not a new launcher or another
container spin-up.

Only the mcp-server-zypp source tree is mounted in; libsolv and libzypp
themselves are never built here. This is a genuine ABI/API compatibility
check against what real Tumbleweed users' systems have.

Usage:
    python3 run_e2e_tests.py [--image IMAGE]
"""
import argparse
import subprocess
import sys
from pathlib import Path

E2E_DIR = Path(__file__).resolve().parent
MCP_SERVER_ZYPP_DIR = E2E_DIR.parents[1]  # .../mcp-server-zypp/tests/e2e -> mcp-server-zypp
CONTAINER_NAME = "zypp-mcp-e2e"
DEFAULT_IMAGE = "opensuse/tumbleweed:latest"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=DEFAULT_IMAGE,
                     help=f"Container image to use (default: {DEFAULT_IMAGE}).")
    args = ap.parse_args()

    subprocess.run(["podman", "rm", "-f", CONTAINER_NAME],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # No :Z/:z relabeling — this container is single-use and never shares
    # these mounts with anything else, so there is nothing to isolate.
    # :Z's private relabel has a real failure mode: if the container is
    # ever force-removed (podman rm -f, e.g. after a mid-run crash) rather
    # than stopped gracefully, the label is not restored, leaving the host
    # directory stuck under a container-private SELinux context. Disabling
    # labeling for this mount avoids that class of problem entirely.
    subprocess.run([
        "podman", "run", "-d", "--name", CONTAINER_NAME,
        "--security-opt", "label=disable",
        "-v", f"{MCP_SERVER_ZYPP_DIR}:/src/mcp-server-zypp",
        "-v", f"{E2E_DIR}:/e2e",
        args.image, "sleep", "infinity",
    ], check=True)

    try:
        print("Installing python3 (not in the base image)...")
        subprocess.run(
            ["podman", "exec", CONTAINER_NAME,
             "zypper", "--non-interactive", "install", "-y", "python3"],
            check=True,
        )

        result = subprocess.run([
            "podman", "exec", CONTAINER_NAME, "python3", "/e2e/container_test.py",
        ])
        sys.exit(result.returncode)
    finally:
        subprocess.run(["podman", "rm", "-f", CONTAINER_NAME],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
