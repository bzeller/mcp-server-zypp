# mcp-server-zypp

An [MCP](https://modelcontextprotocol.io/) (Model Context Protocol) server that lets LLM agents
query and manage packages on an openSUSE/SLES system via [libzypp](https://github.com/openSUSE/libzypp) —
search, dependency lookup, update checks, and gated install/removal with human-in-the-loop
approval for GPG key trust and license acceptance.

## What it does

The server exposes eight tools over MCP:

| Tool | Root required | Purpose |
|---|---|---|
| `search_packages` | no | Find packages by name/summary |
| `find_providers` | no | Which packages provide a given capability |
| `find_dependents` | no | Which packages depend on a given package |
| `check_updates` | no | List available updates |
| `plan_install` | no | Dry-run solve for an install — no changes made |
| `plan_remove` | no | Dry-run solve for a removal — no changes made |
| `confirm_install` | **yes** | Actually install, gated on GPG key trust and license acceptance |
| `confirm_remove` | **yes** | Actually remove |

The two `confirm_*` tools are the only ones that touch the system, and refuse to run unless the
worker process is root. `plan_*` and the read-only tools are always available and never modify
anything.

Two safety properties worth knowing about, since they shape the code:

- **GPG key trust decisions are elicitation-only.** There is no tool argument that can pre-approve
  a key fingerprint — an LLM agent can only accept or reject a key through an interactive MCP
  elicitation round-trip, never by passing an argument that bypasses the prompt.
- **`confirm_install`/`confirm_remove` are cancellable up until the RPM transaction itself
  starts**, and never interrupted once it has (interrupting a live RPM transaction is a good way
  to corrupt the local package database).

## Code structure: Go proxy + C++ worker

The project is two binaries with a strict division of responsibility:

```
mcp-server-zypp (Go, proxy/)          zypp-mcp-tool (C++, worker/)
─────────────────────────────         ──────────────────────────────
Speaks MCP over stdio (or             Actually calls libzypp; owns
optionally HTTP, debug only)          all package-management logic
Spawns a fresh zypp-mcp-tool          One-shot process per tool call
subprocess per tool call              (no long-lived libzypp state
                                       carried between calls)
Forwards elicitation/progress          Emits elicitation requests and
frames to the real MCP client          progress frames the proxy relays
Is a protocol bridge — makes no        Owns every decision about what a
decisions about payload content        tool call means and how it fails
```

**Why a subprocess-per-call, not a library.** `zypp-mcp-tool` links directly against libzypp and
is spawned fresh for every single tool invocation, then exits. This keeps the Go proxy simple and
memory-safe (it never touches libzypp's C++ ABI) and sidesteps libzypp's own reentrancy
constraints — there is no risk of one tool call's librarystate leaking into the next.

**Wire protocol.** The proxy and worker talk over the worker's stdin/stdout using
Content-Length-framed JSON, LSP-style (see `worker/src/transport.h`/`.cc`). The one exception is
tool discovery: `zypp-mcp-tool --list-tools` prints plain JSON to stdout with no framing at all —
it's meant to be usable like any other Unix tool, piped into `jq` etc.

**Tool schemas live in C++, not Go.** Every tool's name, description, JSON input schema, and
root-requirement flag is defined once, in `worker/src/tools/registry.h` plus one `.cc` per tool
under `worker/src/tools/`. The Go proxy has no hardcoded tool list: at startup it runs
`--list-tools` against every `zypp-mcp-*` binary it finds in its worker directory and registers
whatever comes back (see `proxy/internal/tools/tools.go: Register`/`discoverWorkers`). Adding a
tool means adding a `.cc` file in the worker and a registry entry — nothing to change in Go.

### Directory layout

```
worker/                    The C++ worker (zypp-mcp-tool)
  src/
    main.cc                Argument parsing, --list-tools, dispatch
    transport.{h,cc}        Content-Length framing over stdin/stdout
    context.{h,cc}          ToolContext: per-invocation state, owns the
                             transport, gates, and lazy libzypp handle
    callbacks.{h,cc}        libzypp callback receivers -> progress/
                             elicitation frames (the biggest file; this
                             is where most libzypp integration lives)
    gpgkeygate.{h,cc}       GPG key trust state, libzypp-type-free
    commitfailurelog.{h,cc} Structured commit error/warning collection,
                             libzypp-type-free
    cancellation.{h,cc}     SIGTERM-based cancellation latch
    system.{h,cc}           Loading the live system / a testcase system
    tools/                  One pair of files per tool (registry.h lists
                             them all); transaction.{h,cc} holds shared
                             commit/license/solver-result helpers
  tests/                   Boost.Test unit tests (see Testing below)
  CMakeLists.txt

proxy/                     The Go proxy (mcp-server-zypp)
  cmd/mcp-server-zypp/      main() + the enable_http build tag split
  internal/
    config/                 Build-time-injected worker directory path
    tools/                   Tool discovery/registration, frame<->MCP
                             translation (progress, elicitation, errors)
    worker/                  Subprocess lifecycle: spawn, frame I/O,
                             graceful SIGTERM->SIGKILL cancellation
  tests/                    Integration tests driving a real worker
                             binary against synthetic solver testcases
  go.mod / go.sum
  CMakeLists.txt

tests/
  testdata/                 Synthetic solver "testcase" fixtures shared
                             by both the C++ unit tests and the Go
                             integration tests (tc-simple, tc-conflict,
                             tc-license, ...)
  e2e/                       Manual-only, podman-based end-to-end suite
                             against a real rpm database (see its own
                             README.md)

cmake/FindGo.cmake          Locates the go toolchain for CMake
mcp-server-zypp.spec        OBS/RPM packaging
mcp-server-zypp.changes     User-facing changelog (one entry per
                             released/tagged version)
```

## Building

### Prerequisites

- CMake >= 3.17
- A C++17 compiler
- libzypp-devel (or build inside `zypp-stack`, which provides an in-tree `zypp` target — see the
  root `AGENTS.md`)
- yaml-cpp-devel
- Go >= 1.23 (only if building the proxy — see below)
- Boost (`unit_test_framework`) for the C++ tests

### Building both binaries

From the `zypp-stack` superproject (recommended — resolves libzypp automatically):

```bash
mkdir build && cd build
cmake ..
make
```

Standalone, against a system-installed libzypp:

```bash
cd mcp-server-zypp
mkdir build && cd build
cmake ..
cmake --build .
```

This builds `worker/zypp-mcp-tool` and, since Go was found, `proxy/mcp-server-zypp` too.

### Building only the C++ worker

Useful when the Go proxy is built separately (e.g. via `go build -mod=vendor` in the RPM spec, or
when driving the worker directly for e2e testing):

```bash
cmake -DBUILD_GO_PROXY=OFF ..
cmake --build .
```

### Enabling the HTTP transport (local debugging only)

The proxy only speaks stdio by default. HTTP has no authentication and must never be exposed
beyond localhost — it exists purely for tools like the MCP Inspector (below) that are easier to
drive over HTTP than stdio:

```bash
cmake -DZYPP_MCP_ENABLE_HTTP=ON ..
cmake --build .
./proxy/mcp-server-zypp -transport=http
```

## Testing

There are three independent layers. None of them require root or a live system, except the
manual e2e suite.

### C++ unit tests (worker)

```bash
ctest --test-dir build -R 'mcp_(worker_cpp|gpgkeygate|commitfailurelog)_tests' -V
```

Three binaries: `zypp-mcp-tool-tests` (needs the full libzypp toolchain; covers logic like the
license gate by calling it directly — actual `commit()`/RPM execution needs root and is
deliberately out of scope here), plus `zypp-mcp-gpgkeygate-tests` and
`zypp-mcp-commitfailurelog-tests`, which build independently of libzypp entirely (both
`GpgKeyGate` and `CommitFailureLog` are libzypp-type-free by design) and so stay fast.

### Go tests (proxy)

```bash
cd proxy
go test ./...
```

Or via ctest, which also builds the worker first:

```bash
ctest --test-dir build -R 'mcp_(worker|proxy_unit)_tests' -V
```

`mcp_worker_tests` spawns a real `zypp-mcp-tool` against the synthetic solver testcases in
`tests/testdata/` (via `MCP_WORKER_BINARY`/`MCP_TESTDATA_DIR`) — no live system, no root, no
network. `mcp_proxy_unit_tests` covers pure logic (e.g. the cancellation latch) against
`io.Pipe()` rather than a real subprocess.

### Manual end-to-end tests (podman, real rpm database)

```bash
python3 tests/e2e/run_e2e_tests.py
```

Builds `zypp-mcp-tool` fresh inside a throwaway container (against that container's own
`libzypp-devel`, so compiler/headers/runtime library are guaranteed to match) and drives it
directly through real GPG key trust and license confirmation scenarios against a real rpm
database. Root, a real RPM/GPG toolchain, and `podman` are the only prerequisites — see
`tests/e2e/README.md` for the full design rationale and how to add a new scenario. Not run by
`ctest`; trigger it yourself when verifying behaviour the other two layers can't reach.

## Trying it out with the MCP Inspector

The [MCP Inspector](https://github.com/modelcontextprotocol/inspector) is the fastest way to
manually exercise the server without wiring up a real LLM client. It needs Node.js; no changes to
this project are required.

### Over stdio (default, no extra build flags)

```bash
npx @modelcontextprotocol/inspector build/proxy/mcp-server-zypp
```

The Inspector spawns the proxy itself and speaks MCP over its stdin/stdout, so this works with the
binary exactly as it will run in production.

### Over HTTP (optional, more convenient for repeated manual testing)

Build with `-DZYPP_MCP_ENABLE_HTTP=ON` (see above), start the proxy, then point the Inspector at
it:

```bash
build/proxy/mcp-server-zypp -transport=http &
npx @modelcontextprotocol/inspector
# In the Inspector UI: choose "Streamable HTTP", connect to
# http://localhost:8080 (or whichever port/path the server printed at startup)
```

Either way, the Inspector's UI lets you list tools, inspect their JSON schemas, invoke them with
arbitrary arguments, and watch elicitation prompts and progress notifications arrive live — useful
for confirming a new tool's schema looks right, or reproducing an elicitation/progress issue
without needing a full LLM agent in the loop.

**Reminder:** `confirm_install`/`confirm_remove` really do modify the system and require the proxy
(and therefore the worker it spawns) to be running as root. Point the Inspector at a disposable
VM or container unless you mean to actually install or remove something.

## Packaging

`mcp-server-zypp.spec` builds the two binaries independently to satisfy OBS's build environment
(the Go proxy is built with `-mod=vendor` against a committed vendor tarball rather than reaching
out to the network — see the spec's comments for the exact reasoning) and installs `zypp-mcp-tool`
into `%_libexecdir/mcp-server-zypp/`, never onto `$PATH`. `mcp-server-zypp.changes` collects one
entry per released (tagged) version, not per commit.
