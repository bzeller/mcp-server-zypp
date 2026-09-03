"""
Shared test fixtures: RPM building, rpm-md repo publishing, throwaway HTTP
serving, and small process helpers. Used by every scenario under
tests/testsuite/*/*.

This is what remains of the old tests/e2e/e2e_common.py once the pieces
that are no longer this module's job are removed:
  - FrameClient / call_tool() -> replaced entirely by lib/mcpclient.py,
    which drives the real proxy over real MCP instead of zypp-mcp-tool
    directly.
  - build_worker() / spec_build_requires() -> moved to
    tests/run-in-container.py (dev-only, not packaged; a from-source
    build is not this package's concern at all once installed).
"""
import contextlib
import functools
import subprocess
import sys
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

LIB_DIR = Path(__file__).resolve().parent


def run(cmd, **kw):
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    try:
        return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)
    except subprocess.CalledProcessError as e:
        # capture_output=True hides the command's own stdout/stderr inside
        # the exception object — CalledProcessError.__str__ prints neither,
        # only "returned non-zero exit status N". Surface both here so a
        # failure is actually diagnosable instead of a bare exit code with
        # no indication of what the command itself reported.
        if e.stdout:
            print(e.stdout, file=sys.stderr)
        if e.stderr:
            print(e.stderr, file=sys.stderr)
        raise


def step(msg):
    print(f"\n=== {msg} ===")


def fail(msg):
    sys.exit(f"FAIL: {msg}")


COMMIT_TEST_SPEC = LIB_DIR / "commit-test-package.spec"


def build_test_rpm(topdir: Path, name: str, version: str, *,
                    requires: str = None, fail_post: bool = False) -> Path:
    """Builds one RPM from commit-test-package.spec, parameterised via
    --define. Mirrors the license scenario's own build_rpm(), against the
    shared spec instead of the license-gate-specific one.

    requires, if given, becomes a Requires: on a capability name the
    caller controls — pass a name nothing provides to force a solver
    failure. fail_post adds a %post that exits 1, which rpm reports as
    a non-fatal scriptlet failure without failing the transaction.
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

    Always use this rather than publishing a bare directory of RPMs: a
    plaindir repo refreshes unconditionally on every system load, which
    would silently defeat any scenario that deletes an RPM after
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
    torn down before the process itself exits — most importantly one
    served over HTTP (see served_over_http() below): both the gpg_key and
    license scenarios call a bare `zypper --non-interactive refresh` (no
    alias, i.e. every enabled repo), so a leftover repo pointing at an
    already-terminated HTTP server would fail every later scenario's
    refresh, not just this one's. Local dir: repos are comparatively
    harmless to leave registered, but removing them too is still good
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
    only ever engages for a downloading URL scheme; a dir: repo can
    never reach it.

    In-process ThreadingHTTPServer rather than a `python3 -m http.server`
    subprocess: binding port 0 lets the kernel assign a free port and
    report it back immediately, so this cannot collide with anything
    else on the same host, and the socket is already listening by the
    time this function returns, so there is no startup race to poll for
    either. Shutdown is deterministic (server.shutdown() blocks until the
    serve loop actually exits), so there is no subprocess wait()/kill()
    dance and nothing that can hang indefinitely on teardown.
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
