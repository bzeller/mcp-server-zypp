"""
Commit-failure diagnostics — confirm_install's structured error/warning
reporting (CommitFailureLog, worker/src/commitfailurelog.h/.cc and
worker/src/tools/transaction.cc: commitFailureToJson()/commitIssuesToJson()).
Previously a commit failure reported only a generic message with no detail
about what actually went wrong or which packages were affected.

Both libzypp backends are pinned explicitly via environment variables
rather than relying on defaults, which are distro- and build-dependent:

  ZYPP_PCK_PRELOAD     pins the commit DOWNLOAD strategy (classic serial
                       vs. parallel preload)
  ZYPP_SINGLE_RPMTRANS pins the RPM TRANSACTION backend (classic vs.
                       SingleTrans)

They are independent of one another. See
.opencode/plans/mcp-e2e-commit-diagnostics.md §3 and §3b for the full
source-verified rationale, including why the preload path additionally
requires an HTTP-served repo (a dir: repo can never reach it) and why
real createrepo_c metadata is mandatory rather than a bare RPM directory.

Cases, in run order — A leaves a clean system; B and C never install
anything (both delete the package's own RPM out from under a published
repo to force a download-time failure); D installs successfully:

  A. A failing %post scriptlet is reported as a "warnings" entry on an
     otherwise-successful install, not a transaction failure. Run twice,
     once per rpm transaction backend — the two reach the same conclusion
     through entirely separate receivers.
  B. A missing package file on the CLASSIC serial download path produces
     a populated commit "details[]" with phase == "download".
  C. The same failure on the PARALLEL PRELOAD path, phase == "preload".
  D. A genuinely successful preload download, asserting that
     McpCommitPreloadReceive::progress() actually fires — something Case
     C's instant 404 miss legitimately never triggers at all (see its
     own comment), so it cannot be asserted there.
"""
import json
from pathlib import Path

from e2e_common import (build_test_rpm, call_tool, fail, publish_rpm_md_repo,
                         remove_repo, run, served_over_http, step)

RPMBUILD_TOPDIR = Path("/tmp/rpmbuild-commit-failure")


def _case_a_scriptlet_warning(backend: str, single_rpmtrans: str, pkg: str, tag: str):
    """One run of Case A against a single, pinned rpm transaction backend.

    The two backends detect the scriptlet failure through entirely
    different receivers, and — contrary to an earlier assumption in this
    file's history — do NOT record it under the same CommitPhase:

    - classic: McpInstallReceive::report() pattern-matches the raw rpm
      output line via looksLikeScriptletFailure() and records it under
      CommitPhase::Install (callbacks.cc:660-665).
    - SingleTrans: %post output is reported transaction-wide via
      McpSingleTransReceive::report() (SendSingleTransReport's
      contentLogline, level "war" — the same level libzypp uses for a
      non-fatal %posttrans failure), recorded under
      CommitPhase::Transaction, not Install (callbacks.cc:738-775). It is
      NOT captured by McpInstallSAReceive at all.
      McpCommitScriptSAReceive::finish() additionally records a *second*,
      CommitPhase::Script entry ("<type> script warning") whose text does
      not contain "scriptlet" and is not a substitute for the above.

    The one invariant genuinely shared by both backends is the raw rpm
    line's text containing "scriptlet failed, " — the phase it lands
    under is backend-specific and asserted per-backend below instead.
    """
    step(f"Case A ({backend}): failing %post scriptlet -> warnings on an "
         f"otherwise-successful install")
    rpm = build_test_rpm(RPMBUILD_TOPDIR, pkg, "1.0", fail_post=True)
    repo_dir = Path(f"/repo-commit-a-{backend}")
    publish_rpm_md_repo(repo_dir, [rpm], f"scriptlet-test-repo-{backend}")

    frames = []
    result = call_tool(
        "confirm_install", {"package": pkg, "repo": f"scriptlet-test-repo-{backend}"},
        progress_sink=frames,
        extra_env={"ZYPP_PCK_PRELOAD": "0", "ZYPP_SINGLE_RPMTRANS": single_rpmtrans},
        log_tag=tag,
    )
    print(json.dumps(result, indent=2))

    # A %post failure must NOT fail the transaction as a whole.
    if result.get("type") != "result":
        fail(f"Case A ({backend}): expected a successful result frame despite "
             f"the scriptlet failure, got {result}")
    run(["rpm", "-q", pkg])

    warnings = result.get("warnings", [])
    if not warnings:
        fail(f"Case A ({backend}): expected a non-empty \"warnings\" array, got none")

    # phase is exact per backend (see the docstring above for why they
    # differ); "scriptlet failed, " in the text is the one invariant held
    # in common and is asserted regardless of phase.
    expected_phase = "install" if backend == "classic" else "transaction"
    if not any(w.get("phase") == expected_phase and "scriptlet failed, " in w.get("text", "")
               for w in warnings):
        fail(f"Case A ({backend}): expected a warning with phase == "
             f"{expected_phase!r} whose text contains \"scriptlet failed, \". "
             f"Got: {warnings}")

    if not any(f.get("action") == "install" for f in frames):
        fail(f"Case A ({backend}): expected at least one progress frame with "
             f"action == \"install\"")

    # SingleTrans additionally records a CommitPhase::Script warning from
    # McpCommitScriptSAReceive::finish() (callbacks.cc:953-958), whose text
    # is "<type> script warning" and notably does NOT contain "scriptlet" —
    # a supplement to the transaction-phase entry above, never a substitute.
    #
    # Deliberately NOT hard-asserted: a libzypp built with
    # LIBZYPP_CONFIG_USE_CLASSIC_RPMTRANS_BY_DEFAULT=1 silently runs classic
    # even with ZYPP_SINGLE_RPMTRANS=1, because singleTransEnabled() then
    # also requires ImZYPPER() — which tests /proc/self/exe's basename for
    # literally "zypper", and ours is "zypp-mcp-tool". Hard-asserting would
    # fail for that environmental reason rather than any defect in the code
    # under test. Report it instead, so the log records which backend really
    # ran. See the plan doc §3b, point 3.
    script_warnings = [w for w in warnings if w.get("phase") == "script"]
    script_frames = [f for f in frames if f.get("action") == "script"]
    print(f"[{backend}] script-phase warnings: {len(script_warnings)}, "
          f"script progress frames: {len(script_frames)} "
          f"(informational — indicates whether SingleTrans actually engaged)")

    step(f"Case A ({backend}): clean up so the next run is a fresh install")
    run(["rpm", "-e", pkg])

    print(f"PASS: Case A ({backend}) — scriptlet failure reported as a "
          f"{expected_phase}-phase warning, install still succeeded")


def _case_b_classic_download_failure():
    step("Case B: missing package file on the classic (serial) download path")
    rpm = build_test_rpm(RPMBUILD_TOPDIR, "download-fail-pkg", "1.0")
    repo_dir = Path("/repo-commit-b")
    # Generated rpm-md metadata is required here, not just convenient — a
    # bare plaindir directory would be re-scanned on the worker's own system
    # load and the deleted package would vanish from the pool entirely,
    # turning this into a solver error instead of the download failure this
    # case exists to test. See the plan doc §3a, point 2.
    publish_rpm_md_repo(repo_dir, [rpm], "download-test-repo")

    # Delete the RPM out from under the still-published metadata. Do NOT
    # re-run createrepo_c afterwards — that would regenerate primary.xml
    # without the package and defeat the whole point of this case.
    (repo_dir / rpm.name).unlink()

    frames = []
    result = call_tool(
        "confirm_install", {"package": "download-fail-pkg", "repo": "download-test-repo"},
        progress_sink=frames,
        # ZYPP_SINGLE_RPMTRANS is pinned purely for reproducibility: this
        # case fails during download, before the rpm transaction begins, so
        # the backend should not matter — pinning removes one source of
        # environmental variation from any failure diagnosis.
        extra_env={"ZYPP_PCK_PRELOAD": "0", "ZYPP_SINGLE_RPMTRANS": "0"},
        log_tag="commit-B",
    )
    print(json.dumps(result, indent=2))

    if result.get("type") != "error":
        fail(f"Case B: expected an error frame, got {result}")
    # COMMIT_FAILED vs COMMIT_PREPARE_FAILED depends on whether commit()
    # returned normally or threw — not something an env var controls, so
    # both are accepted here.
    if result.get("code") not in ("COMMIT_FAILED", "COMMIT_PREPARE_FAILED"):
        fail(f"Case B: expected COMMIT_FAILED or COMMIT_PREPARE_FAILED, "
             f"got {result.get('code')!r}")

    details = result.get("details", [])
    if not details:
        fail("Case B: expected a non-empty \"details\" array — this is the "
             "actual regression guard for the original bug report")
    if not any(d.get("severity") == "error" and d.get("phase") == "download"
               for d in details):
        fail(f"Case B: expected a detail with severity == \"error\" and "
             f"phase == \"download\" (exact — pinned via ZYPP_PCK_PRELOAD=0), "
             f"got: {details}")

    # Informational only — bucketing depends on step stage, not asserted.
    print("skipped/failed installs (informational):",
          result.get("skipped_installs"), result.get("failed_installs"))

    print("PASS: Case B — classic download failure reported with non-empty "
          "details[], phase == \"download\"")


def _case_c_preload_failure():
    step("Case C: missing package file on the parallel preload path")
    rpm = build_test_rpm(RPMBUILD_TOPDIR, "preload-fail-pkg", "1.0")
    repo_dir = Path("/repo-commit-c")

    # createrepo_c is mandatory here (not merely convenient as it is for a
    # local dir: repo): plaindir is never probed for an http:// URL, and the
    # <checksum> it generates is what makes a package eligible for preload
    # at all. See the plan doc §3a point 1, and §3 point 4.
    repo_dir.mkdir(parents=True, exist_ok=True)
    run(["cp", str(rpm), str(repo_dir / rpm.name)])
    run(["createrepo_c", str(repo_dir)])

    with served_over_http(repo_dir) as base_url:
        run(["zypper", "--non-interactive", "addrepo", "--no-gpgcheck",
             base_url, "preload-test-repo"])
        try:
            run(["zypper", "--non-interactive", "refresh", "preload-test-repo"])

            # Delete the RPM out from under the still-served, still-published
            # metadata — the HTTP server now 404s for it. Do NOT re-run
            # createrepo_c afterwards, for the same reason as Case B.
            (repo_dir / rpm.name).unlink()

            frames = []
            result = call_tool(
                "confirm_install", {"package": "preload-fail-pkg", "repo": "preload-test-repo"},
                progress_sink=frames,
                extra_env={"ZYPP_PCK_PRELOAD": "1", "ZYPP_SINGLE_RPMTRANS": "0"},
                log_tag="commit-C",
            )
            print(json.dumps(result, indent=2))

            if result.get("type") != "error":
                fail(f"Case C: expected an error frame, got {result}")

            details = result.get("details", [])
            if not details:
                fail("Case C: expected a non-empty \"details\" array")
            if not any(d.get("severity") == "error" and d.get("phase") == "preload"
                       for d in details):
                fail(f"Case C: expected a detail with severity == \"error\" and "
                     f"phase == \"preload\" (exact — pinned via ZYPP_PCK_PRELOAD=1), "
                     f"got: {details}")

            if not any(f.get("action") == "preload" for f in frames):
                fail("Case C: expected at least one progress frame with "
                     "action == \"preload\"")
        finally:
            # Mandatory, not just tidy: preload-test-repo points at an
            # HTTP server that is about to be torn down when this `with`
            # block exits. Both gpg_key.py and license.py later call a
            # bare `zypper refresh` (every enabled repo, no alias) — if
            # this repo were left registered, every later scenario's
            # refresh would fail trying to reach a dead server, not just
            # this one (this was caught by a real e2e run: gpg_key and
            # license both failed immediately after commit_failure ran,
            # with no error of their own). Runs even if an assertion
            # above failed, via the enclosing try/finally.
            remove_repo("preload-test-repo")

    print("PASS: Case C — preload download failure reported with non-empty "
          "details[], phase == \"preload\"")


def _case_d_preload_progress_engaged():
    """A genuinely successful preload install (no deleted RPM, package
    stays installed) — the counterpart to Case C's failure. Asserts that
    McpCommitPreloadReceive::progress() actually fires during a real
    preload download, which Case C legitimately cannot: per
    commitpackagepreloader.cc, progress() is only ever invoked from
    onRequestProgress(), which itself only fires once the network layer
    has real response bytes — a 404 miss never reaches that state, but a
    real, successful, nonzero-size download does, and its first byte
    report is unconditional (bypasses the dispatcher's 500ms progress
    throttle — see reportBytesDownloaded()), so this is a deterministic
    guarantee, not a timing gamble.

    Only occurrence is asserted here (did progress() fire at all) — the
    exact bytes_received/bytes_required key-name mapping this depends on
    is covered deterministically, against a synthetic UserData, by
    worker/tests/preloadprogress_test.cc instead.
    """
    step("Case D: successful preload download -> progress() actually fires")
    rpm = build_test_rpm(RPMBUILD_TOPDIR, "preload-progress-pkg", "1.0")
    repo_dir = Path("/repo-commit-d")

    repo_dir.mkdir(parents=True, exist_ok=True)
    run(["cp", str(rpm), str(repo_dir / rpm.name)])
    run(["createrepo_c", str(repo_dir)])

    with served_over_http(repo_dir) as base_url:
        run(["zypper", "--non-interactive", "addrepo", "--no-gpgcheck",
             base_url, "preload-progress-repo"])
        try:
            run(["zypper", "--non-interactive", "refresh", "preload-progress-repo"])

            frames = []
            result = call_tool(
                "confirm_install",
                {"package": "preload-progress-pkg", "repo": "preload-progress-repo"},
                progress_sink=frames,
                extra_env={"ZYPP_PCK_PRELOAD": "1", "ZYPP_SINGLE_RPMTRANS": "0"},
                log_tag="commit-D",
            )
            print(json.dumps(result, indent=2))

            if result.get("type") != "result":
                fail(f"Case D: expected a successful result frame, got {result}")
            run(["rpm", "-q", "preload-progress-pkg"])

            if not any(f.get("action") == "preload" and "percent" in f for f in frames):
                fail(f"Case D: expected at least one preload progress frame "
                     f"with a \"percent\" key (i.e. progress() fired), got "
                     f"frames: {frames}")
        finally:
            remove_repo("preload-progress-repo")

    print("PASS: Case D — preload progress() fired during a real download")


def run_scenario():
    _case_a_scriptlet_warning("classic", "0", "scriptlet-fail-classic", "commit-A-classic")
    _case_a_scriptlet_warning("satrans", "1", "scriptlet-fail-satrans", "commit-A-satrans")
    _case_b_classic_download_failure()
    _case_c_preload_failure()
    _case_d_preload_progress_engaged()
