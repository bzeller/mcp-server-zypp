"""
Shared helpers for e2e scenario modules (see scenarios/) — the worker
build step, the Content-Length framed client used to drive zypp-mcp-tool
directly (no real MCP client in this loop), and small process/output
helpers. Runs inside the podman container as root; imported by
container_test.py and by every scenarios/*.py module.
"""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SRC_DIR = Path("/src/mcp-server-zypp")
BUILD_DIR = Path("/build")
WORKER = str(BUILD_DIR / "worker" / "zypp-mcp-tool")  # cmake mirrors SRC_DIR's layout
E2E_DIR = Path(__file__).resolve().parent

ZYPP_HEAD_REPO = "https://download.opensuse.org/repositories/zypp:/Head/openSUSE_Tumbleweed/"

HEADER_RE = re.compile(rb"Content-Length: (\d+)\r\n\r\n")


def run(cmd, **kw):
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)


def step(msg):
    print(f"\n=== {msg} ===")


def fail(msg):
    sys.exit(f"FAIL: {msg}")


class FrameClient:
    """Minimal Content-Length framed client, mirroring McpTransport's wire
    format (worker/src/transport.cc) exactly: "Content-Length: N\\r\\n\\r\\n"
    followed by N raw bytes. Reads one byte at a time for the header, same
    approach the worker itself uses, since the header is only a few dozen
    bytes and simplicity here matters more than throughput."""

    def __init__(self, cmd, env=None):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      env=env)
        self._stdin_open = True

    def read_frame(self):
        buf = b""
        while not buf.endswith(b"\r\n\r\n"):
            b = self.proc.stdout.read(1)
            if not b:
                return None  # EOF
            buf += b
        m = HEADER_RE.match(buf)
        if not m:
            raise RuntimeError(f"Malformed frame header: {buf!r}")
        length = int(m.group(1))
        payload = self.proc.stdout.read(length)
        return json.loads(payload)

    def write_frame(self, obj):
        if not self._stdin_open:
            return
        payload = json.dumps(obj).encode()
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode()
        self.proc.stdin.write(header + payload)
        self.proc.stdin.flush()

    def close_stdin(self):
        if self._stdin_open:
            self.proc.stdin.close()
            self._stdin_open = False

    def wait(self):
        self.close_stdin()
        return self.proc.wait()


def call_tool(tool: str, args: dict, elicitation_answers=None,
              log_tag: str = "call") -> dict:
    """Invokes a zypp-mcp-tool tool, acting as the elicitation client
    ourselves — there is no real MCP client in this loop.

    elicitation_answers maps an elicitation "method" name to the answer
    string to send back. A method not present in it (or if the dict is
    omitted, e.g. for tools that never elicit) gets no stdin write at
    all — closing stdin instead, simulating a client that never answers
    (no elicitation support, or one that silently drops the request).
    The commit_active zypp_control handshake (see ZYppCallbacks.h:
    CommitActiveReport) is always acked, since it only fires once
    anything gating the commit has already been resolved and a real
    install/removal is about to happen.
    """
    elicitation_answers = elicitation_answers or {}
    log_path = Path(f"/tmp/zypp-mcp-tool-{log_tag}-{os.getpid()}-{id(args)}.log")
    env = os.environ.copy()
    env["ZYPP_LOGFILE"] = str(log_path)  # libzypp's own internal trace,
    # separate from this worker's own stdout JSON protocol channel
    # (ZYPP_LOGFILE="-" would go to stderr, not stdout — see LogControl.cc —
    # but a dedicated file per call keeps each invocation's trace distinct).

    client = FrameClient([WORKER, "--tool", tool, "--arg", json.dumps(args)], env=env)
    try:
        while True:
            frame = client.read_frame()
            if frame is None:
                rc = client.wait()
                sys.exit(f"FAIL: worker exited without a final frame (exit code {rc})")

            t = frame.get("type")
            if t == "elicitation":
                method = frame.get("method")
                print(f"[elicitation] method={method!r} data={frame.get('data')!r}",
                      file=sys.stderr)
                if method in elicitation_answers:
                    client.write_frame({"answer": elicitation_answers[method]})
                else:
                    client.close_stdin()
            elif t == "zypp_control" and frame.get("event") == "commit_active":
                client.write_frame({"ack": True})
            elif t in ("result", "error"):
                client.wait()
                return frame
            # progress / commit_finished / anything else: ignore, keep reading
    finally:
        if log_path.exists():
            print(f"--- libzypp internal log: {log_path} ---", file=sys.stderr)
            print(log_path.read_text(), file=sys.stderr)
            print("--- end log ---", file=sys.stderr)
            log_path.unlink(missing_ok=True)


def spec_build_requires() -> list:
    """Extract package names from mcp-server-zypp.spec's BuildRequires
    lines — the single source of truth for what this project needs to
    build, rather than a hand-maintained list here that can drift out of
    sync with it."""
    project_spec = SRC_DIR / "mcp-server-zypp.spec"
    names = [
        m.group(1)
        for line in project_spec.read_text().splitlines()
        if (m := re.match(r"BuildRequires:\s*(\S+)", line))
    ]
    if not names:
        fail(f"No BuildRequires found in {project_spec} — can't determine build deps.")
    return names


def build_worker():
    """Adds zypp:Head, upgrades to it in lockstep, installs build tooling
    from the project's own spec, and builds zypp-mcp-tool fresh against
    this container's own libzypp-devel. Shared setup for every scenario
    suite — run exactly once per container, never per scenario."""
    step("Add the zypp:Head OBS repo (ahead of stock Tumbleweed — this "
         "worker may depend on a libzypp API not yet in Factory)")
    run(["zypper", "--non-interactive", "addrepo",
         "--refresh", "--priority", "90", ZYPP_HEAD_REPO, "zypp-head"])
    # --gpg-auto-import-keys here is trusting SUSE's own build service repo
    # as test setup infrastructure, same category as the base image's own
    # preconfigured repos — unrelated to, and not to be confused with, any
    # deliberately never-imported throwaway key a scenario sets up itself.
    run(["zypper", "--non-interactive", "--gpg-auto-import-keys", "refresh"])

    # A plain `zypper install libzypp-devel` can be satisfied by the
    # ALREADY-installed, older stock runtime libzypp (pulled in as a base
    # image dependency of zypper itself) — RPM dependency resolution only
    # requires *some* package providing the right soname, not necessarily
    # the newest one available. That leaves libzypp-devel's headers newer
    # than the runtime .so they actually link against: an "undefined
    # reference" at link time for any API only in the newer headers.
    # `zypper dup --from` instead upgrades everything already installed
    # that zypp-head also provides — runtime libzypp included — to that
    # repo's versions in one lockstep operation, before libzypp-devel is
    # ever installed, so headers and runtime library are guaranteed to
    # come from the same build.
    step("Upgrade to zypp:Head's versions in lockstep (runtime + devel together)")
    run(["zypper", "--non-interactive", "dup",
         "--from", "zypp-head", "--allow-vendor-change"])

    step("Install build tooling (from the spec's BuildRequires) "
         "+ signing/repo tooling for e2e scenarios")
    run(["zypper", "--non-interactive", "install", "-y",
         *spec_build_requires(), "rpm-build", "gpg2", "createrepo_c"])

    step("Configure zypp-mcp-tool against this container's own libzypp-devel")
    BUILD_DIR.mkdir(exist_ok=True)
    # Only the C++ worker is needed for e2e tests — BUILD_GO_PROXY=OFF skips
    # the Go proxy build entirely (go itself is still installed above, since
    # it's a real BuildRequires per the spec; just not invoked here).
    configure = subprocess.run(
        ["cmake", "-S", str(SRC_DIR), "-B", str(BUILD_DIR),
         "-DCMAKE_BUILD_TYPE=Debug", "-DBUILD_GO_PROXY=OFF"],
        text=True, capture_output=True,
    )
    if configure.returncode != 0:
        print(configure.stdout, file=sys.stderr)
        print(configure.stderr, file=sys.stderr)
        sys.exit(
            "FAIL: cmake configure failed against this container's "
            "libzypp-devel. This may mean the container doesn't ship "
            "libzypp-devel's CMake config where find_package(Zypp) "
            "expects it, or another build dependency is missing — see "
            "the output above."
        )

    step("Build zypp-mcp-tool")
    build = subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", "zypp-mcp-tool",
         "--parallel", str(os.cpu_count() or 1)],
        text=True, capture_output=True,
    )
    if build.returncode != 0:
        print(build.stdout, file=sys.stderr)
        print(build.stderr, file=sys.stderr)
        sys.exit(
            "INCOMPATIBLE: zypp-mcp-tool failed to build against this "
            "container's libzypp-devel. This likely means the worker "
            "depends on a libzypp API not yet in this distro package, "
            "not a bug in this test — see the compiler output above."
        )
    print(f"Built {WORKER}")

    step("Smoke-test: does the freshly built binary actually run?")
    proc = subprocess.run([WORKER, "--list-tools"], capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr, file=sys.stderr)
        fail(f"zypp-mcp-tool built but --list-tools failed (exit {proc.returncode})")
