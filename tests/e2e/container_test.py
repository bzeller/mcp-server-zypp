#!/usr/bin/env python3
"""
Runs inside the podman container as root. Builds zypp-mcp-tool once
against this container's own libzypp-devel package (see e2e_common.
build_worker() — only mcp-server-zypp is compiled here, never libsolv/
libzypp themselves), then discovers and runs every scenario module under
scenarios/ in this same container/build — a new e2e scenario needs a new
scenarios/<name>.py file, not a new container or another build.

Building fresh inside the container rather than mounting a host-built
binary means the compiler, headers and runtime libzypp.so all come from
the exact same package — no risk of a soname mismatch between a locally
built binary and whatever this container's Tumbleweed snapshot happens to
ship (a real, previously-hit failure mode: a binary linked against
libboost_thread.so.1.91.0 on the host has no guarantee a freshly pulled
"latest" rolling-release image still ships that exact version). If the
container's libzypp-devel doesn't yet expose some API a scenario needs,
the build itself fails with a clear compiler error naming it.
"""
import importlib
import pkgutil
import sys

from e2e_common import build_worker, step

import scenarios


def main():
    build_worker()

    results = {}
    for _, name, _ in sorted(pkgutil.iter_modules(scenarios.__path__)):
        step(f"Scenario suite: {name}")
        mod = importlib.import_module(f"scenarios.{name}")
        try:
            mod.run_scenario()
            results[name] = True
        except KeyboardInterrupt:
            raise
        except BaseException as e:
            # BaseException (not just Exception) deliberately, so a
            # scenario's own sys.exit()/fail() (SystemExit) is caught
            # here too — one suite failing should not prevent the others
            # from running and reporting their own results.
            print(f"SCENARIO SUITE '{name}' FAILED: {e}", file=sys.stderr)
            results[name] = False

    step("Summary")
    for name, ok in results.items():
        print(f"{'PASS' if ok else 'FAIL'}: {name}")

    if not all(results.values()):
        sys.exit(1)
    print("\nALL PASS")


if __name__ == "__main__":
    main()
