# End-to-end tests

Manual-only integration tests that need root, a real RPM/GPG toolchain, and a
throwaway container. Not part of `ctest` and not run automatically — trigger
them yourself when you want to verify behaviour that can't be exercised by
the unit test suites.

## Prerequisites

- [`podman`](https://podman.io/) on the host. Nothing else — `zypp-mcp-tool`
  is built fresh inside the container itself (see below), not on the host.

## Running

```bash
python3 tests/e2e/run_e2e_tests.py

# Use a different base image (default: opensuse/tumbleweed:latest):
python3 tests/e2e/run_e2e_tests.py --image opensuse/tumbleweed:latest
```

This spins up **one** throwaway container, builds `zypp-mcp-tool` **once**,
and runs **every** scenario module under `scenarios/` inside it — adding a
new scenario means adding a new `scenarios/<name>.py` file, not spinning up
another container or building the code again.

## Layout

| File | Runs where | Role |
|---|---|---|
| `run_e2e_tests.py` | host | Spins up the container, mounts in `mcp-server-zypp/` + this `e2e/` directory, execs `container_test.py`. |
| `container_test.py` | container, as root | Builds the worker once (`e2e_common.build_worker()`), discovers every `scenarios/*.py` module, runs each's `run_scenario()`, prints a pass/fail summary. |
| `e2e_common.py` | container | Shared helpers: the build step, `FrameClient` (a minimal Content-Length framed client standing in for a real MCP client — see below), `call_tool()`, `run()`/`step()`/`fail()`. |
| `scenarios/*.py` | container | One file per scenario suite. Each defines `run_scenario()`, called by `container_test.py`. Self-contained — sets up its own repo/packages/keys under its own paths, independent of other scenarios. |

Nothing from the host is built or copied in beyond the source tree itself.

## How the build works

`container_test.py`'s `build_worker()` (in `e2e_common.py`):

1. Adds the [`zypp:Head`](https://build.opensuse.org/project/show/zypp:Head)
   OBS repo, ahead of stock Tumbleweed/Factory — this worker may depend on
   a libzypp API that hasn't landed in Factory yet. Then `zypper dup
   --from zypp-head` to upgrade everything already installed (runtime
   libzypp included, pulled in as a base-image dependency of `zypper`
   itself) to that repo's versions in one lockstep operation — a plain
   `zypper install libzypp-devel` can otherwise be satisfied by the older
   stock runtime library already present (RPM dependency resolution only
   needs *some* package providing the right soname), leaving the newer
   `-devel` headers mismatched against an older runtime `.so` — an
   "undefined reference" at link time for any API only in the newer
   headers. Then installs build tooling from `mcp-server-zypp.spec`'s own
   `BuildRequires` (parsed directly from the spec file, so this never
   drifts out of sync with the project's real dependencies) plus
   `rpm-build`/`gpg2`/`createrepo_c` for the scenarios themselves.
2. Configures and builds **only** `zypp-mcp-tool` (`-DBUILD_GO_PROXY=OFF`)
   against the resulting `libzypp-devel` package — `find_package(Zypp
   REQUIRED)` in `worker/CMakeLists.txt` picks it up via CMake's standard
   config-mode search, no special path setup needed. Building fresh like
   this, rather than mounting a host-built binary, guarantees the
   compiler, headers and runtime `libzypp.so` all come from the exact same
   package: there is no risk of a soname mismatch between a binary built
   on the host at some point in the past and whatever a freshly pulled
   rolling-release image happens to ship *today* (a real failure mode this
   replaced — a host build linked against `libboost_thread.so.1.91.0` has
   no guarantee a newly pulled image still has that exact version). If the
   container's `libzypp-devel` doesn't yet expose some API a scenario
   needs, the **build itself** fails with a clear compiler error naming
   it — a more precise diagnostic than a runtime "missing symbol" would
   have been, and reported distinctly from an actual gate-logic failure.

## How scenarios talk to zypp-mcp-tool

There is no real MCP client in this loop — scenarios invoke `zypp-mcp-tool`
directly via `e2e_common.call_tool()`, which acts as a minimal stand-in:
`FrameClient` speaks the same Content-Length framing as the real transport
(`worker/src/transport.cc`), answers `"elicitation"` frames according to
the scenario's own script, and always acks the `commit_active` handshake
(declining that would test cancellation, not whatever the scenario is
actually about).

## Scenarios

### `gpg_key`: GPG key trust handling

`confirm_install`'s key trust decisions go through MCP elicitation only —
there is no tool argument that can pre-approve a key (see
`worker/src/gpgkeygate.h`). Verifies that end to end against a real signed
RPM and a real, never-imported GPG key:

1. No answer to the key-trust elicitation (stdin closed) — simulates a
   client without elicitation support. Must be denied and reported as
   `KEY_NOT_TRUSTED` with the correct fingerprint.
2. An explicit decline — same expected outcome.
3. An explicit accept — must actually install the package.

Setup notes:

- The repo is added with `addrepo --no-gpgcheck`, **persisted** into the
  `.repo` file (`gpgcheck=0`), not just a one-off flag on a single
  `zypper` invocation. That matters because `zypp-mcp-tool` reads the same
  `.repo` file in its own, separate process later: a global,
  per-invocation `--no-gpg-checks` only gets the container's own `zypper`
  past the unsigned metadata once, while the repo's persisted setting
  would stay at the (checked) default — so `zypp-mcp-tool`'s later,
  independent load would hit the exact same unsigned-metadata prompt
  itself, with nothing scripted to answer it, aborting before ever
  reaching the package-level key check this scenario is actually about (a
  real failure this replaced).
- Since `--no-gpgcheck` also disables *package*-level checking
  (`RepoInfo::pkgGpgCheck()` ORs in the general gpgcheck flag),
  `pkg_gpgcheck=1` is appended to the `.repo` file explicitly afterward,
  decoupling it back on — package signatures stay checked, which is
  exactly what this scenario needs to exercise.
- An explicit `gpgkey=` URL (pointing at the exported public key) is also
  appended to the `.repo` file. Without it,
  `provideAndImportKeyFromRepository` can't even fetch the key material
  to ask whether to trust it (it tries the conventional
  `repodata/repomd.xml.key` auto-discovery path by default, which doesn't
  exist here) — the commit aborts one step before the key-trust
  elicitation this scenario is about is ever reached.

### `license`: license confirmation gate

`confirm_install`'s `accepted_licenses` argument (unlike GPG keys, this
really is a plain tool argument, not elicitation — see
`worker/src/tools/transaction.h: checkLicensesAccepted`). Verifies end to
end against a real, hand-crafted **susetags** repo:

1. No `accepted_licenses` on a fresh install — must require confirmation,
   reporting the exact license text and a `license_id`.
2. That `license_id` supplied — must install.
3. Upgrading an already-installed package to a version with **identical**
   license text — must proceed without requiring re-confirmation at all
   (mirrors zypper's own `confirm_licenses` behaviour, bnc#394396; already
   covered by a C++ unit test against a testcase — this exercises the same
   behaviour against a real, live rpm database).

Why susetags instead of rpm-md/`createrepo_c`: a package's license/EULA
attribute (`SolvAttr::eula`) is populated from a repo metadata `<eula>` tag
that `createrepo_c` does not generate from a plain RPM — it's a
manually-curated field, not derived from RPM headers. Patching generated,
checksummed `primary.xml`/`other.xml` by hand to inject it is fragile and
would need `repomd.xml`'s checksums regenerated too. susetags supports the
same thing as a plain, hand-authorable `=Eul:` tag in a text file instead —
the same mechanism already proven by the C++ unit test fixtures under
`tests/testdata/tc-license*`.

The test package is left completely unsigned with `gpgcheck` disabled
entirely — this scenario is deliberately independent of GPG key handling.
An unsigned package with `pkgGpgCheckIsMandatory() == false` is silently
accepted by `packageSigCheck()` (`CHK_NOSIG` relaxed to `CHK_OK`) — no
elicitation, no abort, regardless of signing.

## Adding another e2e scenario

Add `scenarios/<name>.py` defining a `run_scenario()` function that raises
(via `e2e_common.fail()`, or lets an unhandled exception propagate) on
failure and returns normally on success. `container_test.py` discovers and
runs it automatically — no launcher or container changes needed. Use
`e2e_common.call_tool()` to drive `zypp-mcp-tool`, and keep the scenario
self-contained (its own repo directory, its own package names) so it can't
collide with any other scenario running in the same container.
