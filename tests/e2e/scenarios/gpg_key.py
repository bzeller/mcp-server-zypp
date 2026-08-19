"""
GPG key trust handling — confirm_install's key trust decisions go through
MCP elicitation only, there is no tool argument that can pre-approve a key
(see worker/src/gpgkeygate.h). Verifies that end to end against a real
signed RPM and a real, never-imported GPG key:

  1. No answer at all for the key-trust elicitation (stdin closed) — this
     is what a client without elicitation support looks like from the
     worker's point of view (the proxy maps that case to "decline", the
     worker maps EOF to the same outcome) — must deny and report
     KEY_NOT_TRUSTED with the correct fingerprint.
  2. An explicit "decline" answer — same expected outcome.
  3. An explicit "accept" answer — must actually install the package.
"""
import json
from pathlib import Path

from e2e_common import E2E_DIR, call_tool, fail, run, step

SPEC = E2E_DIR / "test-package.spec"
GNUPGHOME = Path("/tmp/gpg-e2e")
RPMBUILD_TOPDIR = Path("/tmp/rpmbuild-gpg-key")
REPO_DIR = Path("/repo-gpg-key")
KEY_UID = "E2E Test Key <e2e@test>"


def check_key_not_trusted(result: dict, fingerprint: str):
    print(json.dumps(result, indent=2))
    if result.get("code") != "KEY_NOT_TRUSTED":
        fail(f"expected KEY_NOT_TRUSTED, got {result.get('code')!r}")
    reported = {k.get("fingerprint", "").upper() for k in result.get("keys", [])}
    if fingerprint.upper() not in reported:
        fail(f"fingerprint {fingerprint} not among reported keys {reported}")
    print("PASS: rejected, correct fingerprint reported")


def run_scenario():
    step("Generate a throwaway GPG signing key (never imported into rpm's keyring)")
    GNUPGHOME.mkdir(mode=0o700, exist_ok=True)
    run(["gpg", "--homedir", str(GNUPGHOME), "--batch", "--passphrase", "",
         "--quick-gen-key", KEY_UID, "rsa2048", "sign", "never"])
    fpr_out = run(["gpg", "--homedir", str(GNUPGHOME),
                   "--with-colons", "--fingerprint", KEY_UID]).stdout
    fingerprint = next(
        line.split(":")[9] for line in fpr_out.splitlines() if line.startswith("fpr:")
    )
    print(f"Generated key fingerprint: {fingerprint}")

    step("Build the dummy test RPM")
    RPMBUILD_TOPDIR.mkdir(parents=True, exist_ok=True)
    run(["rpmbuild", "-bb", str(SPEC), "--define", f"_topdir {RPMBUILD_TOPDIR}"])
    rpm_path = next(RPMBUILD_TOPDIR.glob("RPMS/**/*.rpm"))
    pkg_name = run(["rpm", "-qp", "--qf", "%{NAME}", str(rpm_path)]).stdout.strip()

    step("Sign the RPM with the throwaway key")
    (Path.home() / ".rpmmacros").write_text(
        "%_signature gpg\n"
        f"%_gpg_name {KEY_UID}\n"
        f"%_gpg_path {GNUPGHOME}\n"
    )
    run(["rpm", "--addsign", str(rpm_path)])

    step("Serve it from a local dir: repo (metadata unchecked, package checks forced on)")
    REPO_DIR.mkdir(exist_ok=True)
    run(["cp", str(rpm_path), str(REPO_DIR)])
    run(["createrepo_c", str(REPO_DIR)])

    # provideAndImportKeyFromRepository (called from rpmSigFileChecker when
    # packageSigCheck reports CHK_NOKEY) needs to actually fetch the key
    # material before there's anything to ask report()/
    # askUserToAcceptPackageKey to trust — an explicit gpgkey= URL is how a
    # real repo declares where to find it (RepoInfo::gpgKeyUrls()), rather
    # than relying on the repodata/repomd.xml.key auto-discovery fallback
    # (RepoInfoWorkflow::fetchGpgKeys). Without this, the fetch itself
    # fails ("not found"), and the whole commit aborts before the
    # key-trust elicitation this test is about is ever reached.
    key_path = REPO_DIR / "mykey.asc"
    key_path.write_text(
        run(["gpg", "--homedir", str(GNUPGHOME), "--armor", "--export", KEY_UID]).stdout
    )

    # --no-gpgcheck is PERSISTED into the .repo file (gpgcheck=0) — needed
    # because zypp-mcp-tool reads that same file in its own, separate
    # process later. A global, per-invocation --no-gpg-checks flag on just
    # this refresh only gets the CONTAINER's zypper past the unsigned
    # metadata once — the repo's persisted setting would stay at the
    # global default (checked), and zypp-mcp-tool's later independent load
    # would hit the exact same unsigned-metadata prompt itself, with
    # nothing scripted to answer it, aborting before ever reaching the
    # package-level key check this test is actually about. No key is
    # involved in the metadata itself at all (it isn't signed), so nothing
    # gets auto-imported or trusted by disabling this check.
    run(["zypper", "--non-interactive", "addrepo", "--no-gpgcheck",
         str(REPO_DIR), "gpg-key-test-repo"])
    # zypper addrepo has no --gpgkey flag — append it to the generated
    # .repo file directly, same as pkg_gpgcheck below. RepoInfo::
    # pkgGpgCheck() ORs in the general gpgcheck flag, so --no-gpgcheck
    # above also silently disables PACKAGE-level signature checking — the
    # one thing this test needs active. Force it back on explicitly,
    # decoupled from the metadata setting.
    with open("/etc/zypp/repos.d/gpg-key-test-repo.repo", "a") as f:
        f.write(f"gpgkey={key_path.as_uri()}\n")
        f.write("pkg_gpgcheck=1\n")
    run(["zypper", "--non-interactive", "refresh"])

    step("Scenario 1: no answer to the key-trust elicitation (stdin closed)")
    result = call_tool(
        "confirm_install", {"package": pkg_name, "repo": "gpg-key-test-repo"},
        elicitation_answers={}, log_tag="gpg-key-1",
    )
    check_key_not_trusted(result, fingerprint)

    step("Scenario 2: explicit decline")
    result = call_tool(
        "confirm_install", {"package": pkg_name, "repo": "gpg-key-test-repo"},
        elicitation_answers={"trust_package_key": "decline"}, log_tag="gpg-key-2",
    )
    check_key_not_trusted(result, fingerprint)

    step("Scenario 3: explicit accept — must install")
    result = call_tool(
        "confirm_install", {"package": pkg_name, "repo": "gpg-key-test-repo"},
        elicitation_answers={"trust_package_key": "accept"}, log_tag="gpg-key-3",
    )
    print(json.dumps(result, indent=2))
    if result.get("type") != "result":
        fail(f"expected a result frame, got {result}")
    run(["rpm", "-q", pkg_name])  # raises if not installed
    print("PASS: accepted and installed")
