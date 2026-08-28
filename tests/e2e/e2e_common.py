"""
Shared helpers for e2e scenario modules (see scenarios/) — the worker
build step, the Content-Length framed client used to drive zypp-mcp-tool
directly (no real MCP client in this loop), and small process/output
helpers. Runs inside the podman container as root; imported by
container_test.py and by every scenarios/*.py module.
"""
import contextlib
import functools
import json
import os
import re
import subprocess
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
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
              log_tag: str = "call", progress_sink=None, extra_env=None) -> dict:
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

    progress_sink, if given, is a list that every "progress" frame seen
    along the way is appended to (opt-in, append-only — omit it and
    progress frames are silently discarded exactly as before this
    parameter existed).

    extra_env, if given, is merged into the worker's environment before
    ZYPP_LOGFILE is applied — this function always controls ZYPP_LOGFILE
    itself, so a caller cannot redirect it via extra_env, and the finally
    block below can always find the log at the path it just set. Example
    use: {"ZYPP_PCK_PRELOAD": "0"} to pin the commit download backend (see
    commitpackagepreloader.cc: preloadEnabled() and
    .opencode/plans/mcp-e2e-commit-diagnostics.md §3 for why this must be
    pinned explicitly rather than relying on the default).
    """
    elicitation_answers = elicitation_answers or {}
    log_path = Path(f"/tmp/zypp-mcp-tool-{log_tag}-{os.getpid()}-{id(args)}.log")
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    # Set last, deliberately: must win over both the inherited environment
    # and extra_env, so the finally block below always finds the log at
    # the exact path recorded here.
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
            elif t == "progress":
                if progress_sink is not None:
                    progress_sink.append(frame)
            elif t in ("result", "error"):
                client.wait()
                return frame
            # commit_finished / anything else: ignore, keep reading
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


COMMIT_TEST_SPEC = E2E_DIR / "commit-test-package.spec"


def build_test_rpm(topdir: Path, name: str, version: str, *,
                    requires: str = None, fail_post: bool = False) -> Path:
    """Builds one RPM from commit-test-package.spec, parameterised via
    --define. Mirrors scenarios/license.py's build_rpm(), against the
    shared spec instead of the license-gate-specific one.

    requires, if given, becomes a Requires: on a capability name the
    caller controls — pass a name nothing provides to force a solver
    failure. fail_post adds a %post that exits 1, which rpm reports as
    a non-fatal scriptlet failure without failing the transaction (see
    .opencode/plans/mcp-e2e-commit-diagnostics.md §5 for the exact
    rpm-reported text this produces).
    """
    topdir.mkdir(parents=True, exist_ok=True)
    cmd = ["rpmbuild", "-bb", str(COMMIT_TEST_SPEC),
           "--define", f"_topdir {topdir}",
           "--define", f"pkg_name {name}",
           "--define", f"pkg_version {version}"]
    if requires:
        cmd += ["--define", f"pkg_requires {requires}"]
    if fail_post:
        cmd += ["--define", "fail_post 1"]
    run(cmd)
    matches = list(topdir.glob(f"RPMS/**/{name}-{version}-1.*.rpm"))
    if not matches:
        fail(f"rpmbuild did not produce an RPM for {name}-{version}")
    return matches[0]


def publish_rpm_md_repo(repo_dir: Path, rpms: list, alias: str, *, base_url: str = None):
    """Copies rpms into repo_dir, generates real rpm-md metadata via
    createrepo_c, then addrepo/refreshes it under zypper.

    Always use this rather than publishing a bare directory of RPMs —
    see .opencode/plans/mcp-e2e-commit-diagnostics.md §3a for why
    generated metadata is required here even though libzypp itself
    would happily accept a plain, metadata-less directory (RPMPLAINDIR):
    a plaindir repo refreshes unconditionally on every system load,
    which would silently defeat any scenario that deletes an RPM after
    publishing while leaving the metadata referencing it.

    base_url, if given, is the URL passed to `zypper addrepo` instead of
    repo_dir itself — this is how the same on-disk metadata generation
    step serves both a local dir: repo and an HTTP-served one (see
    served_over_http() below). Metadata generation always happens
    on-disk regardless of which URL is ultimately used.
    """
    repo_dir.mkdir(parents=True, exist_ok=True)
    for rpm in rpms:
        run(["cp", str(rpm), str(repo_dir / rpm.name)])
    run(["createrepo_c", str(repo_dir)])
    run(["zypper", "--non-interactive", "addrepo", "--no-gpgcheck",
         base_url or str(repo_dir), alias])
    run(["zypper", "--non-interactive", "refresh", alias])


def remove_repo(alias: str):
    """Best-effort `zypper removerepo`, tolerant of the repo already being
    gone. Use in a finally block for any repo whose backing storage is
    torn down before the container itself exits — most importantly one
    served over HTTP (see served_over_http() below): both gpg_key.py and
    license.py call a bare `zypper --non-interactive refresh` (no alias,
    i.e. every enabled repo), so a leftover repo pointing at an
    already-terminated HTTP server would fail every later scenario's
    refresh, not just this one's. Local dir: repos are comparatively
    harmless to leave registered (their backing directory persists for
    the life of the container), but removing them too is still good
    hygiene and costs nothing.
    """
    result = subprocess.run(
        ["zypper", "--non-interactive", "removerepo", alias],
        text=True, capture_output=True,
    )
    if result.returncode != 0:
        print(f"[remove_repo] {alias}: {result.stderr.strip()} (ignored)", file=sys.stderr)


@contextlib.contextmanager
def served_over_http(directory: Path):
    """Serves directory over HTTP on an ephemeral 127.0.0.1 port for the
    duration of the context; yields the base URL. Used to exercise the
    parallel preload download path, which — unlike the classic path —
    only ever engages for a downloading URL scheme (see
    commitpackagepreloader.cc and
    .opencode/plans/mcp-e2e-commit-diagnostics.md §3, point 3); a dir:
    repo can never reach it.

    In-process ThreadingHTTPServer rather than a `python3 -m http.server`
    subprocess: binding port 0 lets the kernel assign a free port and
    report it back immediately, so this cannot collide with anything
    else on the same host (no fixed port to hardcode, no time-of-check/
    time-of-use gap from picking one ourselves first) — and the socket
    is already listening by the time this function returns, so there is
    no startup race to poll for either. Shutdown is deterministic
    (server.shutdown() blocks until the serve loop actually exits), so
    there is no subprocess wait()/kill() dance and nothing that can hang
    indefinitely on teardown.
    """
    handler = functools.partial(SimpleHTTPRequestHandler, directory=str(directory))
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        if thread.is_alive():
            fail("served_over_http: server thread did not exit after shutdown()")


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
