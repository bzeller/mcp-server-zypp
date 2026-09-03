#!/usr/bin/env python3
"""
Host-side launcher: spins up a single throwaway podman container, builds
mcp-server-zypp fresh inside it against that container's own libzypp-devel
(both the C++ worker and, unlike the old e2e suite, the Go proxy — see
below), then runs tests/testsuite/run-testsuite against that build.

DEV ONLY — not packaged, not installed. The packaged, production
entry point is tests/testsuite/run-testsuite itself, run directly against
an installed mcp-server-zypp package. This script exists purely so
unmerged, in-tree changes can be tested the same way, without needing a
disposable VM.

Adapted from the old tests/e2e/run_e2e_tests.py + e2e_common.build_worker(),
with the changes needed now that the *proxy* is the thing under test, not
just the worker:

1. BUILD_GO_PROXY=ON (was OFF) — the old e2e suite drove zypp-mcp-tool
   directly and never built the proxy at all; this suite drives the real
   proxy over real MCP, so it must exist.
2. The built proxy is invoked with -worker-dir pointing at the build
   tree's worker directory, since a from-source, uninstalled build has no
   %{_libexecdir}/mcp-server-zypp for the proxy's baked-in
   DefaultWorkerDir (CMAKE_INSTALL_FULL_LIBEXECDIR, only correct once
   actually installed) to find.

Still adds the zypp:Head OBS repo and upgrades to it in lockstep before
building — this remains a genuine ABI/API compatibility canary against
real Tumbleweed snapshots, exactly as it was for the worker-only suite.

Usage:
    python3 tests/run-in-container.py [--image IMAGE] [scenario]
"""
import argparse
import subprocess
import sys
from pathlib import Path

MCP_SERVER_ZYPP_DIR = Path(__file__).resolve().parents[1]  # .../mcp-server-zypp/tests -> mcp-server-zypp
CONTAINER_NAME = "zypp-mcp-testsuite"
DEFAULT_IMAGE = "opensuse/tumbleweed:latest"
BUILD_DIR = "/build"
ZYPP_HEAD_REPO = "https://download.opensuse.org/repositories/zypp:/Head/openSUSE_Tumbleweed/"

# The container-internal build script — templated once and run via
# `podman exec ... sh -c`, rather than maintaining a second .py file that
# would need its own mount, since every step here is a handful of shell
# commands anyway (mirrors e2e_common.build_worker()'s steps 1:1, plus
# the proxy build BUILD_GO_PROXY=ON added).
BUILD_SCRIPT = f"""
set -e
zypper --non-interactive install -y python3

echo "=== Add the zypp:Head OBS repo ==="
zypper --non-interactive addrepo --refresh --priority 90 {ZYPP_HEAD_REPO} zypp-head
zypper --non-interactive --gpg-auto-import-keys refresh

echo "=== Upgrade to zypp:Head's versions in lockstep (runtime + devel together) ==="
zypper --non-interactive dup --from zypp-head --allow-vendor-change

echo "=== Install build tooling (spec BuildRequires) + test tooling ==="
BUILDREQ=$(grep -oP '^BuildRequires:\\s*\\K\\S+' /src/mcp-server-zypp/mcp-server-zypp.spec)
zypper --non-interactive install -y $BUILDREQ rpm-build gpg2 createrepo_c python3-mcp

echo "=== Configure (BUILD_GO_PROXY=ON — the proxy is under test here) ==="
cmake -S /src/mcp-server-zypp -B {BUILD_DIR} -DCMAKE_BUILD_TYPE=Debug -DBUILD_GO_PROXY=ON

echo "=== Build ==="
cmake --build {BUILD_DIR} --parallel "$(nproc)"
"""


def main():
    # Force line-buffered stdout regardless of whether it's a TTY — see
    # the identical comment in tests/testsuite/run-testsuite for why this
    # matters: every subprocess spawned below inherits our stdout fd and
    # writes to it immediately, so without this our own print() calls
    # (block-buffered once stdout isn't a TTY, e.g. redirected to a file)
    # can appear AFTER output from a subprocess that was started later.
    sys.stdout.reconfigure(line_buffering=True)

    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=DEFAULT_IMAGE,
                     help=f"Container image to use (default: {DEFAULT_IMAGE}).")
    ap.add_argument("scenario", nargs="?",
                     help="Run only this one scenario, passed through to run-testsuite.")
    args = ap.parse_args()

    subprocess.run(["podman", "rm", "-f", CONTAINER_NAME],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # No :Z/:z relabeling — see the identical rationale in the old
    # run_e2e_tests.py: this container is single-use, never shares these
    # mounts with anything else, and a mid-run podman rm -f would
    # otherwise leave the host directory stuck under a container-private
    # SELinux context.
    subprocess.run([
        "podman", "run", "-d", "--name", CONTAINER_NAME,
        "--security-opt", "label=disable",
        "-v", f"{MCP_SERVER_ZYPP_DIR}:/src/mcp-server-zypp",
        args.image, "sleep", "infinity",
    ], check=True)

    try:
        print("=== Building mcp-server-zypp (worker + proxy) inside container ===")
        subprocess.run(
            ["podman", "exec", CONTAINER_NAME, "sh", "-c", BUILD_SCRIPT],
            check=True,
        )

        # Source tree (with tests/testsuite/) is mounted at
        # /src/mcp-server-zypp; run-testsuite is the packaged/production
        # entry point, run here unmodified against the fresh build.
        run_testsuite_cmd = ["/src/mcp-server-zypp/tests/testsuite/run-testsuite"]
        if args.scenario:
            run_testsuite_cmd.append(args.scenario)

        # mcp-server-zypp's own top-level CMakeLists.txt add_subdirectory()s
        # worker/ and proxy/ directly (no extra nesting) — a standalone
        # `-B /build` here lands both binaries straight under /build/.
        env_prefix = [
            "env",
            "MCP_TESTSUITE_DESTRUCTIVE=1",
            f"MCPSERVER={BUILD_DIR}/proxy/mcp-server-zypp",
            f"MCPSERVER_ARGS=-worker-dir={BUILD_DIR}/worker",
        ]

        result = subprocess.run([
            "podman", "exec", CONTAINER_NAME, *env_prefix, *run_testsuite_cmd,
        ])
        sys.exit(result.returncode)
    finally:
        subprocess.run(["podman", "rm", "-f", CONTAINER_NAME],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
