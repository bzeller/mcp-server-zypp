# Testsuite

Integration tests that drive the real, installed `mcp-server-zypp` proxy and `zypp-mcp-tool`
worker over the actual MCP protocol (stdio, JSON-RPC, elicitation, progress notifications) — not
the worker directly. They need root, a real RPM/GPG toolchain, and a real rpmdb, and are packaged
separately as the `mcp-server-zypp-testsuite` subpackage (see `../../mcp-server-zypp.spec`) rather
than run via `ctest`.

**Destructive.** These scenarios install and remove real packages on whatever system runs them.
`run-testsuite` refuses to start unless `MCP_TESTSUITE_DESTRUCTIVE=1` is set explicitly — only
ever set it in a disposable VM or container.

## Prerequisites

- An installed `mcp-server-zypp` + `mcp-server-zypp-testsuite` package (or an in-tree build — see
  "Developer usage" below).
- `python3-mcp`, `rpm-build`, `createrepo_c`, `gpg2` — all `Requires:` of the `testsuite`
  subpackage, so a real install pulls them in automatically.

## Running

```bash
# packaged: every scenario
MCP_TESTSUITE_DESTRUCTIVE=1 /usr/lib/mcp-server-zypp/testsuite/run-testsuite

# packaged: a single scenario, e.g. re-running one openQA failure
MCP_TESTSUITE_DESTRUCTIVE=1 /usr/lib/mcp-server-zypp/testsuite/run-testsuite gpg_key

# developer: throwaway podman container, builds from source, BUILD_GO_PROXY=ON
python3 tests/run-in-container.py
python3 tests/run-in-container.py gpg_key   # single scenario

# advanced: against an already-built tree, no container
MCPSERVER=/build/proxy/mcp-server-zypp \
MCPSERVER_ARGS=-worker-dir=/build/worker \
MCP_TESTSUITE_DESTRUCTIVE=1 \
  tests/testsuite/run-testsuite
```

`run-testsuite` discovers scenarios by directory listing (not a hardcoded list), runs each even
if an earlier one fails, and prints a `PASS`/`FAIL` summary with a non-zero exit if anything
failed.

## Layout

| Path | Runs where | Role |
|---|---|---|
| `run-testsuite` | wherever installed | Discovers and runs every scenario, prints a summary. The packaged, production entry point. |
| `lib/mcpclient.py` | scenario process | Real MCP-over-stdio client: spawns the proxy, drives `tools/call`, answers elicitation requests, collects progress notifications. `call_tool()` is the one function every scenario calls. |
| `lib/fixtures.py` | scenario process | RPM building, rpm-md repo publishing, throwaway HTTP serving, small process helpers. |
| `lib/*.spec` | data | RPM spec templates driven by `lib/fixtures.py`'s `build_test_rpm()` / scenario-specific builders. |
| `<name>/<name>` | scenario process | One executable per scenario, directly runnable on its own for debugging a single failure. |
| `../run-in-container.py` | host, dev only | **Not packaged.** Spins up a throwaway podman container, builds the worker *and* proxy fresh inside it, then runs `run-testsuite` against that build — the only way to test unmerged changes. |

Each scenario directory contains exactly one executable, named after the directory
(`gpg_key/gpg_key`, not `gpg_key/gpg_key.py`) — this matches the `for test in ...; do "./$test";
done` invocation convention `run-testsuite` uses, and lets a scenario be run standalone
(`./gpg_key/gpg_key`) against `$MCPSERVER`/`$MCPSERVER_ARGS` while debugging.

## How scenarios talk to the server

`lib/mcpclient.py`'s `call_tool(tool, args, elicitation_answers=None, progress_sink=None,
extra_env=None)` spawns `$MCPSERVER` (defaulting to the installed `/usr/bin/mcp-server-zypp`) over
real MCP-over-stdio via `python3-mcp`, issues one `tools/call`, and returns the worker's parsed
JSON result frame — the same shape every scenario assertion already expects.

Two things are worth knowing about the real protocol, as opposed to driving the worker directly:

- **Elicitation.** The worker's `{method, data}` elicitation frame is flattened by the proxy into
  one display string, `"[method] data"` (`proxy/internal/tools/tools.go`) — `mcpclient.py` parses
  the method name back out of that prefix to match it against `elicitation_answers`. A method not
  present there is declined, simulating a client without elicitation support.
- **Progress.** `progress_sink`, if given, collects `{"progress", "total", "message"}` dicts — the
  MCP-level shape the proxy derives from the worker's raw progress frame, not the worker's own
  `action`/`percent` fields. Match on `message` content (e.g. `message.startswith("preload")`) to
  identify a frame's origin; see `commit_failure/commit_failure` for worked examples.

## Scenarios

### `tools`: tool registration

Connects, calls `tools/list`, and asserts exactly the expected tool set — 6 tools always, plus
`confirm_install`/`confirm_remove` only when the *server process* runs as root (the proxy's
root gate is applied once at startup, based on the server's own uid, not the client's). No repos,
no rpmdb, the fastest scenario in the suite — and the only one that can catch a proxy built with
the wrong baked-in worker directory (an installed proxy silently registering zero tools), since
that class of bug only exists in an installed binary's baked-in path, not anything a from-source
build exercises.

### `gpg_key`: GPG key trust handling

`confirm_install`'s key trust decisions go through MCP elicitation only — there is no tool
argument that can pre-approve a key (see `worker/src/gpgkeygate.h`). Verifies that end to end
against a real signed RPM and a real, never-imported GPG key:

1. No answer to the key-trust elicitation — must be denied and reported as `KEY_NOT_TRUSTED` with
   the correct fingerprint.
2. An explicit decline — same expected outcome.
3. An explicit accept — must actually install the package.

### `license`: license confirmation gate

`confirm_install`'s `accepted_licenses` argument (unlike GPG keys, a plain tool argument, not
elicitation — see `worker/src/tools/transaction.h: checkLicensesAccepted`). Verifies end to end
against a real, hand-crafted susetags repo (chosen because a package's license/EULA attribute is
populated from a repo metadata `<eula>` tag that `createrepo_c` never generates from a plain RPM):

1. No `accepted_licenses` on a fresh install — must require confirmation, reporting the exact
   license text and a `license_id`.
2. That `license_id` supplied — must install.
3. Upgrading an already-installed package to a version with **identical** license text — must
   proceed without requiring re-confirmation at all (mirrors zypper's own `confirm_licenses`
   behaviour, bnc#394396).

### `solver_error`: solver problem detail

`plan_install`'s `SOLVER_ERROR` response should carry each problem's full detail text and its
proposed solutions, not just a one-line description. Solves against a package with a `Requires:`
on a capability nothing provides, and asserts at least one problem carries a non-empty
`solutions` array. Solve-only, no commit — independent of every other scenario here.

### `commit_failure`: structured commit diagnostics

`confirm_install`'s structured error/warning reporting (`CommitFailureLog`). Five cases:

- **A** (run twice, once per rpm transaction backend): a failing `%post` scriptlet is reported as
  a `warnings` entry on an otherwise-successful install, not a transaction failure.
- **B**: a missing package file on the classic serial download path produces a populated commit
  `details[]` with `phase == "download"`.
- **C**: the same failure on the parallel preload path, `phase == "preload"`.
- **D**: a genuinely successful preload download, asserting that preload progress reporting
  actually fires with a non-zero total — something Case C's instant 404 miss can never trigger.

Both the rpm transaction backend (`ZYPP_SINGLE_RPMTRANS`) and the download backend
(`ZYPP_PCK_PRELOAD`) are pinned explicitly via environment variables passed through `extra_env`,
since their defaults are distro- and libzypp-build-dependent.

## Adding another scenario

Create `<name>/<name>` (executable, no extension) defining a `run_scenario()` function, called
when the file is run directly via `if __name__ == "__main__": run_scenario()`. Use
`lib.mcpclient.call_tool()` to drive the server and `lib.fixtures` for RPM/repo setup. Keep the
scenario self-contained (its own repo directory, its own package names) so it can't collide with
another scenario running in the same suite — `run-testsuite` discovers it automatically, no
launcher or spec changes needed beyond the directory itself already being packaged (see
`%files testsuite` in `../../mcp-server-zypp.spec`, which owns the whole `testsuite/` tree).
