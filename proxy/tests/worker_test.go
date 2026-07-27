// Package tests contains integration tests for zypp-mcp-tool.
// Tests run the worker binary against synthetic solver testcases — no live
// system, no root, no network required.
//
// Required environment variables (set by CMake via add_test):
//   MCP_WORKER_BINARY  - path to the zypp-mcp-tool binary
//   MCP_TESTDATA_DIR   - path to the tests/testdata directory
package tests

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/openSUSE/mcp-server-zypp/internal/worker"
)

// ─── Helpers ─────────────────────────────────────────────────────────────────

func workerBinary(t *testing.T) string {
	t.Helper()
	bin := os.Getenv("MCP_WORKER_BINARY")
	if bin == "" {
		t.Skip("MCP_WORKER_BINARY not set — skipping integration tests")
	}
	return bin
}

func testdataDir(t *testing.T) string {
	t.Helper()
	dir := os.Getenv("MCP_TESTDATA_DIR")
	if dir == "" {
		// Fall back to relative path when running via `go test ./tests/...`
		// from the proxy/ directory.
		dir = filepath.Join("..", "..", "tests", "testdata")
	}
	return dir
}

func testcase(t *testing.T, name string) string {
	t.Helper()
	return filepath.Join(testdataDir(t), name)
}

// arg builds a JSON argument string from string key-value pairs.
func arg(fields map[string]string) string {
	b, _ := json.Marshal(fields)
	return string(b)
}

// argAny builds a JSON argument string from mixed-type key-value pairs.
func argAny(fields map[string]any) string {
	b, _ := json.Marshal(fields)
	return string(b)
}

// invokeSimple runs the worker and returns the parsed result map.
// Fails the test if the worker returns an error frame.
func invokeSimple(t *testing.T, tool, argJSON string) map[string]any {
	t.Helper()
	// Use Invoke directly so we can log the raw frame on failure.
	result, err := worker.Invoke(context.Background(), workerBinary(t), tool, argJSON, nil)
	if err != nil {
		t.Fatalf("worker invoke error: %v", err)
	}
	if result.Type == "error" {
		t.Fatalf("worker returned error: code=%q detail=%q raw=%s",
			result.Code, result.Detail, string(result.Raw))
	}
	var m map[string]any
	if err := json.Unmarshal(result.Raw, &m); err != nil {
		t.Fatalf("unmarshal result: %v\nraw: %s", err, string(result.Raw))
	}
	return m
}

// invokeExpectError runs the worker and asserts it returns an error frame
// with the given code.
func invokeExpectError(t *testing.T, tool, argJSON, expectedCode string) map[string]any {
	t.Helper()
	result, err := worker.Invoke(context.Background(), workerBinary(t), tool, argJSON, nil)
	if err != nil {
		t.Fatalf("invoke failed unexpectedly: %v", err)
	}
	if result.Type != "error" {
		t.Fatalf("expected error frame, got type=%q", result.Type)
	}
	if result.Code != expectedCode {
		t.Fatalf("expected error code %q, got %q (detail: %s)", expectedCode, result.Code, result.Detail)
	}
	var m map[string]any
	json.Unmarshal(result.Raw, &m)
	return m
}

// packages extracts the packages array from a search result.
func packages(t *testing.T, result map[string]any) []map[string]any {
	t.Helper()
	raw, ok := result["packages"].([]any)
	if !ok {
		t.Fatalf("result has no packages array: %v", result)
	}
	out := make([]map[string]any, 0, len(raw))
	for _, p := range raw {
		if m, ok := p.(map[string]any); ok {
			out = append(out, m)
		}
	}
	return out
}

// findPackage returns the first package with the given name, or nil.
func findPackage(pkgs []map[string]any, name string) map[string]any {
	for _, p := range pkgs {
		if p["name"] == name {
			return p
		}
	}
	return nil
}

// planPackages extracts the to_install list from an install/remove plan result.
func planInstall(t *testing.T, result map[string]any) []map[string]any {
	t.Helper()
	return planList(t, result, "to_install")
}

func planRemove(t *testing.T, result map[string]any) []map[string]any {
	t.Helper()
	return planList(t, result, "to_remove")
}

func planList(t *testing.T, result map[string]any, key string) []map[string]any {
	t.Helper()
	plan, ok := result["plan"].(map[string]any)
	if !ok {
		t.Fatalf("result has no plan: %v", result)
	}
	raw, ok := plan[key].([]any)
	if !ok {
		return nil
	}
	out := make([]map[string]any, 0, len(raw))
	for _, p := range raw {
		if m, ok := p.(map[string]any); ok {
			out = append(out, m)
		}
	}
	return out
}

// ─── --list-tools ─────────────────────────────────────────────────────────────

func TestListTools_ReturnsAllTools(t *testing.T) {
	descs, err := worker.ListTools(context.Background(), workerBinary(t))
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}
	names := make(map[string]bool)
	for _, d := range descs {
		names[d.Name] = true
	}
	for _, expected := range []string{"search_packages", "find_providers", "find_dependents", "check_updates", "install_package", "remove_package"} {
		if !names[expected] {
			t.Errorf("tool %q not in --list-tools output", expected)
		}
	}
}

func TestListTools_SchemasHaveTestcaseField(t *testing.T) {
	descs, err := worker.ListTools(context.Background(), workerBinary(t))
	if err != nil {
		t.Fatalf("ListTools: %v", err)
	}
	for _, d := range descs {
		var schema map[string]any
		if err := json.Unmarshal(d.InputSchema, &schema); err != nil {
			t.Errorf("tool %q: unmarshal schema: %v", d.Name, err)
			continue
		}
		props, ok := schema["properties"].(map[string]any)
		if !ok {
			t.Errorf("tool %q: schema has no properties", d.Name)
			continue
		}
		if _, ok := props["testcase"]; !ok {
			t.Errorf("tool %q: schema missing 'testcase' property", d.Name)
		}
	}
}

// ─── search_packages ───────────────────────────────────────────────────────────

func TestSearch_FindsAvailablePackage(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-available", "testcase": tc}))

	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-available")
	if p == nil {
		t.Fatal("pkg-available not found in results")
	}
	if p["status"] != "not-installed" {
		t.Errorf("expected status=not-installed, got %v", p["status"])
	}
	if p["repo"] != "repo-oss" {
		t.Errorf("expected repo=repo-oss, got %v", p["repo"])
	}
}

func TestSearch_FindsInstalledPackage(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-installed", "testcase": tc}))

	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-installed")
	if p == nil {
		t.Fatal("pkg-installed not found in results")
	}
	// Default (details=false) path uses Selectable — status reflects overall install state.
	status, _ := p["status"].(string)
	if status != "user-installed" {
		t.Errorf("expected user-installed status for pkg-installed, got %q", status)
	}
}

func TestSearch_SubstringMatch(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-", "testcase": tc}))

	pkgs := packages(t, result)
	if len(pkgs) < 2 {
		t.Errorf("expected at least 2 results for 'pkg-', got %d", len(pkgs))
	}
}

func TestSearch_NoResults(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "does-not-exist-xyz", "testcase": tc}))

	pkgs := packages(t, result)
	if len(pkgs) != 0 {
		t.Errorf("expected 0 results, got %d", len(pkgs))
	}
}

// ─── check_updates ────────────────────────────────────────────────────────────

func TestCheckUpdates_FindsPatch(t *testing.T) {
	// Use applicable_only=false to find all patches regardless of need state.
	// testtags patches have no category/severity so we only verify structure.
	tc := testcase(t, "tc-updates")
	result := invokeSimple(t, "check_updates",
		argAny(map[string]any{"applicable_only": false, "testcase": tc}))

	patches, ok := result["patches"].([]any)
	if !ok {
		t.Fatalf("no patches array in result: %v", result)
	}
	if len(patches) == 0 {
		t.Fatal("expected at least one patch in tc-updates")
	}
	p := patches[0].(map[string]any)
	if p["name"] == nil || p["name"] == "" {
		t.Error("patch missing name field")
	}
	if p["edition"] == nil {
		t.Error("patch missing edition field")
	}
	if _, ok := p["issues"]; !ok {
		t.Error("patch missing issues array")
	}
}

func TestCheckUpdates_ApplicableOnlyDefault(t *testing.T) {
	// Default (applicable_only=true) on tc-simple which has no update repo
	// should return zero patches.
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "check_updates",
		arg(map[string]string{"testcase": tc}))

	patches, ok := result["patches"].([]any)
	if !ok {
		t.Fatalf("no patches array in result: %v", result)
	}
	if len(patches) != 0 {
		t.Errorf("expected 0 patches in tc-simple, got %d", len(patches))
	}
}

// ─── install_package ──────────────────────────────────────────────────────────

func TestInstall_PlanContainsRequestedPackage(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "install_package",
		arg(map[string]string{"package": "pkg-available", "testcase": tc}))

	toInstall := planInstall(t, result)
	if findPackage(toInstall, "pkg-available") == nil {
		t.Error("pkg-available not in to_install plan")
	}
}

func TestInstall_PlanIncludesDependencies(t *testing.T) {
	tc := testcase(t, "tc-deps")
	result := invokeSimple(t, "install_package",
		arg(map[string]string{"package": "pkg-with-dep", "testcase": tc}))

	toInstall := planInstall(t, result)
	if findPackage(toInstall, "pkg-with-dep") == nil {
		t.Error("pkg-with-dep not in to_install plan")
	}
	if findPackage(toInstall, "pkg-dep") == nil {
		t.Error("pkg-dep (dependency) not in to_install plan")
	}
}

func TestInstall_ConflictReturnsSolverError(t *testing.T) {
	// pkg-a is installed, pkg-b conflicts with pkg-a
	tc := testcase(t, "tc-conflict")
	m := invokeExpectError(t, "install_package",
		arg(map[string]string{"package": "pkg-b", "testcase": tc}),
		"SOLVER_ERROR")

	problems, ok := m["problems"].([]any)
	if !ok || len(problems) == 0 {
		t.Error("expected non-empty problems list in SOLVER_ERROR")
	}
}

func TestInstall_MissingDependencyReturnsSolverError(t *testing.T) {
	tc := testcase(t, "tc-no-solution")
	invokeExpectError(t, "install_package",
		arg(map[string]string{"package": "pkg-unsolvable", "testcase": tc}),
		"SOLVER_ERROR")
}

func TestInstall_AlreadyInstalledProducesEmptyPlan(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "install_package",
		arg(map[string]string{"package": "pkg-installed", "testcase": tc}))

	toInstall := planInstall(t, result)
	if len(toInstall) != 0 {
		t.Errorf("expected empty to_install for already-installed package, got %d items", len(toInstall))
	}
}

// ─── remove_package ───────────────────────────────────────────────────────────

func TestRemove_PlanContainsRequestedPackage(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "remove_package",
		arg(map[string]string{"package": "pkg-installed", "testcase": tc}))

	toRemove := planRemove(t, result)
	if findPackage(toRemove, "pkg-installed") == nil {
		t.Error("pkg-installed not in to_remove plan")
	}
}

func TestRemove_NotInstalledProducesEmptyPlan(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "remove_package",
		arg(map[string]string{"package": "pkg-available", "testcase": tc}))

	toRemove := planRemove(t, result)
	if len(toRemove) != 0 {
		t.Errorf("expected empty to_remove for not-installed package, got %d items", len(toRemove))
	}
}

// ─── Error handling ───────────────────────────────────────────────────────────

func TestInvalidTestcasePath(t *testing.T) {
	invokeExpectError(t, "search_packages",
		arg(map[string]string{"pattern": "pkg", "testcase": "/nonexistent/path/xyz"}),
		"EXCEPTION")
}

func TestNotATestcaseDirectory(t *testing.T) {
	// Pass a real directory that is not a testcase
	invokeExpectError(t, "search_packages",
		arg(map[string]string{"pattern": "pkg", "testcase": os.TempDir()}),
		"EXCEPTION")
}

func TestUnknownTool(t *testing.T) {
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"nonexistent_tool", "", nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "UNKNOWN_TOOL" {
		t.Errorf("expected UNKNOWN_TOOL error, got type=%q code=%q", result.Type, result.Code)
	}
}

func TestMalformedArg(t *testing.T) {
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"search_packages", "not valid json {{{", nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "INVALID_ARG" {
		t.Errorf("expected INVALID_ARG error, got type=%q code=%q", result.Type, result.Code)
	}
}

// ─── Frame routing ────────────────────────────────────────────────────────────

func TestFrames_ResultIsLastFrame(t *testing.T) {
	// Collect all frames from a simple search and verify the last one is result/error
	tc := testcase(t, "tc-simple")

	var frames []string
	_, err := worker.Invoke(context.Background(), workerBinary(t),
		"search_packages",
		arg(map[string]string{"pattern": "pkg-available", "testcase": tc}),
		func(frame json.RawMessage) []byte {
			var env struct{ Type string `json:"type"` }
			json.Unmarshal(frame, &env)
			frames = append(frames, env.Type)
			return nil
		})
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	// Progress frames (if any) must come before the result
	for i, f := range frames {
		if f == "result" || f == "error" {
			if i != len(frames)-1 {
				t.Errorf("result/error frame at position %d, expected last (total=%d)", i, len(frames))
			}
		}
	}
}

// ─── install_package: new argument tests ──────────────────────────────────────

func TestInstall_RepoRestriction_InstallsLesserVersion(t *testing.T) {
	// repo-stable has versioned-pkg 1-0, repo-bleeding has 2-0.
	// Without --repo the solver picks 2-0 (higher version).
	// With --repo repo-stable it must install 1-0 even though 2-0 is available.
	tc := testcase(t, "tc-two-repos")

	// Without repo: expect version 2-0 (solver prefers higher)
	result := invokeSimple(t, "install_package",
		arg(map[string]string{"package": "versioned-pkg", "testcase": tc}))
	toInstall := planInstall(t, result)
	p := findPackage(toInstall, "versioned-pkg")
	if p == nil {
		t.Fatal("versioned-pkg not in plan")
	}
	if p["edition"] != "2-0" {
		t.Errorf("expected edition 2-0 without repo restriction, got %v", p["edition"])
	}

	// With repo-stable: must install 1-0 despite 2-0 being available elsewhere
	result = invokeSimple(t, "install_package",
		arg(map[string]string{"package": "versioned-pkg", "repo": "repo-stable", "testcase": tc}))
	toInstall = planInstall(t, result)
	p = findPackage(toInstall, "versioned-pkg")
	if p == nil {
		t.Fatal("versioned-pkg not in plan with repo-stable restriction")
	}
	if p["edition"] != "1-0" {
		t.Errorf("expected edition 1-0 with repo-stable restriction, got %v", p["edition"])
	}
}

func TestInstall_RepoRestriction_NotFoundInRepo(t *testing.T) {
	// versioned-pkg does not exist in a nonexistent repo.
	tc := testcase(t, "tc-two-repos")
	invokeExpectError(t, "install_package",
		arg(map[string]string{"package": "versioned-pkg", "repo": "repo-nonexistent", "testcase": tc}),
		"NOT_FOUND")
}

func TestInstall_CapabilityFalse_NameBased(t *testing.T) {
	// capability=false: find by exact name — should succeed the same as default
	// but use name-based pool lookup path.
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "install_package",
		argAny(map[string]any{"package": "pkg-available", "capability": false, "testcase": tc}))
	toInstall := planInstall(t, result)
	if findPackage(toInstall, "pkg-available") == nil {
		t.Error("pkg-available not in to_install plan with capability=false")
	}
}

// ─── remove_package: new argument tests ──────────────────────────────────────

func TestRemove_CapabilityTrue_ConflictBased(t *testing.T) {
	// capability=true: uses addConflict path.
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "remove_package",
		argAny(map[string]any{"package": "pkg-installed", "capability": true, "testcase": tc}))
	toRemove := planRemove(t, result)
	if findPackage(toRemove, "pkg-installed") == nil {
		t.Error("pkg-installed not in to_remove plan with capability=true")
	}
}

// ─── Validation error tests ───────────────────────────────────────────────────

func TestInstall_InvalidType(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"install_package",
		arg(map[string]string{"package": "pkg-available", "type": "invalid-type", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for invalid type, got type=%q code=%q", result.Type, result.Code)
	}
	if result.Detail == "" {
		t.Error("expected non-empty detail for invalid type error")
	}
}

func TestInstall_EmptyPackage(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"install_package",
		arg(map[string]string{"package": "", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for empty package, got type=%q code=%q", result.Type, result.Code)
	}
}

func TestInstall_EmptyRepo(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"install_package",
		arg(map[string]string{"package": "pkg-available", "repo": "", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for empty repo, got type=%q code=%q", result.Type, result.Code)
	}
}

func TestRemove_InvalidType(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"remove_package",
		arg(map[string]string{"package": "pkg-installed", "type": "badtype", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for invalid type, got type=%q code=%q", result.Type, result.Code)
	}
}

// ─── search_packages: new mode tests ─────────────────────────────────────────

func TestSearch_ExactMatch(t *testing.T) {
	tc := testcase(t, "tc-simple")
	// exact match: "pkg-available" should find exactly 1 package
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-available", "match": "exact", "testcase": tc}))
	pkgs := packages(t, result)
	if len(pkgs) != 1 {
		t.Errorf("expected exactly 1 result for exact match, got %d", len(pkgs))
	}
}

func TestSearch_ExactMatchNoResult(t *testing.T) {
	tc := testcase(t, "tc-simple")
	// "pkg-" is not an exact package name — should find nothing
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-", "match": "exact", "testcase": tc}))
	pkgs := packages(t, result)
	if len(pkgs) != 0 {
		t.Errorf("expected 0 results for exact match 'pkg-', got %d", len(pkgs))
	}
}

func TestSearch_InstalledOnly(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		argAny(map[string]any{"pattern": "pkg-", "installed_only": true, "testcase": tc}))
	pkgs := packages(t, result)
	for _, p := range pkgs {
		status, _ := p["status"].(string)
		if status != "user-installed" && status != "auto-installed" &&
			status != "other-version-user" && status != "other-version-auto" {
			t.Errorf("installed_only=true returned non-installed package %v (status=%q)", p["name"], status)
		}
	}
	if len(pkgs) == 0 {
		t.Error("expected at least one installed package")
	}
}

func TestSearch_NotInstalledOnly(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		argAny(map[string]any{"pattern": "pkg-", "not_installed_only": true, "testcase": tc}))
	pkgs := packages(t, result)
	for _, p := range pkgs {
		status, _ := p["status"].(string)
		if status == "user-installed" || status == "auto-installed" {
			t.Errorf("not_installed_only=true returned installed package %v (status=%q)", p["name"], status)
		}
	}
}

func TestSearch_ByProvides(t *testing.T) {
	// pkg-available provides some-capability — search_in=provides should find it
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "some-capability", "search_in": "provides", "testcase": tc}))
	pkgs := packages(t, result)
	if findPackage(pkgs, "pkg-available") == nil {
		t.Error("expected pkg-available (which provides some-capability) in results")
	}
}

func TestSearch_InstalledAndNotInstalledMutuallyExclusive(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"search_packages",
		argAny(map[string]any{"pattern": "pkg", "installed_only": true, "not_installed_only": true, "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for mutually exclusive flags, got type=%q code=%q",
			result.Type, result.Code)
	}
}

// ─── find_providers ───────────────────────────────────────────────────────────

func TestFindProviders_FindsProvider(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "find_providers",
		arg(map[string]string{"capability": "pkg-available", "testcase": tc}))
	pkgs := packages(t, result)
	if findPackage(pkgs, "pkg-available") == nil {
		t.Error("expected pkg-available as provider of pkg-available")
	}
}

func TestFindProviders_FindsByCapability(t *testing.T) {
	// some-capability is provided by pkg-available (not the package name)
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "find_providers",
		arg(map[string]string{"capability": "some-capability", "testcase": tc}))
	pkgs := packages(t, result)
	if findPackage(pkgs, "pkg-available") == nil {
		t.Error("expected pkg-available as provider of some-capability")
	}
}

func TestFindProviders_NotFound(t *testing.T) {
	tc := testcase(t, "tc-simple")
	invokeExpectError(t, "find_providers",
		arg(map[string]string{"capability": "nonexistent-cap-xyz", "testcase": tc}),
		"NOT_FOUND")
}

func TestFindProviders_EmptyCapability(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"find_providers",
		arg(map[string]string{"capability": "", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for empty capability, got type=%q code=%q",
			result.Type, result.Code)
	}
}

// ─── find_dependents ──────────────────────────────────────────────────────────

func TestFindDependents_FindsRequirer(t *testing.T) {
	// pkg-with-dep requires pkg-dep — find_dependents on pkg-dep should return pkg-with-dep
	tc := testcase(t, "tc-deps")
	result := invokeSimple(t, "find_dependents",
		arg(map[string]string{"package": "pkg-dep", "relation": "requires", "testcase": tc}))
	pkgs := packages(t, result)
	if findPackage(pkgs, "pkg-with-dep") == nil {
		t.Error("expected pkg-with-dep as a package that requires pkg-dep")
	}
}

func TestFindDependents_DefaultRelationIsRequires(t *testing.T) {
	// omitting relation should default to requires
	tc := testcase(t, "tc-deps")
	result := invokeSimple(t, "find_dependents",
		arg(map[string]string{"package": "pkg-dep", "testcase": tc}))
	pkgs := packages(t, result)
	if findPackage(pkgs, "pkg-with-dep") == nil {
		t.Error("expected pkg-with-dep with default relation=requires")
	}
}

func TestFindDependents_NotFound_PackageDoesNotExist(t *testing.T) {
	tc := testcase(t, "tc-deps")
	invokeExpectError(t, "find_dependents",
		arg(map[string]string{"package": "nonexistent-pkg-xyz", "testcase": tc}),
		"NOT_FOUND")
}

func TestFindDependents_ExactMatchDefault(t *testing.T) {
	// exact=true (default): "pkg-dep" finds only pkg-dep, not pkg-dep-of-dep etc.
	tc := testcase(t, "tc-deps")
	result := invokeSimple(t, "find_dependents",
		arg(map[string]string{"package": "pkg-dep", "testcase": tc}))
	// Verify seed_count=1 — only pkg-dep matched, not pkg-dep-of-dep
	if result["seed_count"] != float64(1) {
		t.Errorf("expected seed_count=1 with exact match, got %v", result["seed_count"])
	}
}

func TestFindDependents_InvalidRelation(t *testing.T) {
	tc := testcase(t, "tc-deps")
	result, err := worker.Invoke(context.Background(), workerBinary(t),
		"find_dependents",
		arg(map[string]string{"package": "pkg-dep", "relation": "badrelation", "testcase": tc}),
		nil)
	if err != nil {
		t.Fatalf("invoke: %v", err)
	}
	if result.Type != "error" || result.Code != "EXCEPTION" {
		t.Errorf("expected EXCEPTION for invalid relation, got type=%q code=%q",
			result.Type, result.Code)
	}
}

// ─── status field ─────────────────────────────────────────────────────────────

// TestSearch_StatusField_Selectable verifies that the default (details=false)
// path emits a "status" string, not a boolean "installed" field.
func TestSearch_StatusField_Selectable(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-", "testcase": tc}))
	pkgs := packages(t, result)
	if len(pkgs) == 0 {
		t.Fatal("expected packages in result")
	}
	for _, p := range pkgs {
		if _, hasOld := p["installed"]; hasOld {
			t.Errorf("package %v still has legacy 'installed' bool field", p["name"])
		}
		status, ok := p["status"].(string)
		if !ok || status == "" {
			t.Errorf("package %v missing or empty 'status' string field", p["name"])
		}
	}
}

// TestSearch_StatusField_Details verifies that details=true also emits "status"
// and the per-solvable fields (edition, arch, repo).
func TestSearch_StatusField_Details(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		argAny(map[string]any{"pattern": "pkg-available", "details": true, "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-available")
	if p == nil {
		t.Fatal("pkg-available not found in details results")
	}
	if _, hasOld := p["installed"]; hasOld {
		t.Error("details path still has legacy 'installed' bool field")
	}
	if p["status"] != "not-installed" {
		t.Errorf("expected status=not-installed, got %v", p["status"])
	}
	// details path must include per-solvable fields
	for _, field := range []string{"edition", "arch", "repo"} {
		if p[field] == nil {
			t.Errorf("details path missing field %q", field)
		}
	}
	if p["repo"] != "repo-oss" {
		t.Errorf("expected repo=repo-oss in details path, got %v", p["repo"])
	}
}

// TestSearch_SelectablePath_HasKindField verifies that the default (details=false)
// path includes a "kind" field and omits nothing essential.
func TestSearch_SelectablePath_HasKindField(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-available", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-available")
	if p == nil {
		t.Fatal("pkg-available not found")
	}
	if p["kind"] == nil {
		t.Error("selectable path missing 'kind' field")
	}
}

// TestSearch_InstalledPackage_RepoNotSystem verifies that an installed package
// whose identical version is available in a repo reports the real repo alias (not @System).
func TestSearch_InstalledPackage_RepoNotSystem(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-installed", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-installed")
	if p == nil {
		t.Fatal("pkg-installed not found")
	}
	repo, _ := p["repo"].(string)
	if repo == "@System" {
		t.Errorf("pkg-installed has identical available in repo-oss — should not report @System, got %q", repo)
	}
	if repo != "repo-oss" {
		t.Errorf("expected repo=repo-oss for pkg-installed, got %q", repo)
	}
}

// TestSearch_OtherVersion_Status verifies that a package installed at a version
// not available in any repo reports status=other-version (or other-version-auto).
func TestSearch_OtherVersion_Status(t *testing.T) {
	// pkg-other-version 1-1 is installed; only 1-0 is in repo-oss — no identical available.
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-other-version", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-other-version")
	if p == nil {
		t.Fatal("pkg-other-version not found")
	}
	status, _ := p["status"].(string)
	if status != "other-version-user" && status != "other-version-auto" {
		t.Errorf("expected other-version or other-version-auto for pkg-other-version, got %q", status)
	}
}

// TestSearch_OtherVersion_RepoFallsBackToSystem verifies that a package with no
// identical available falls back to @System for its repo field.
func TestSearch_OtherVersion_RepoFallsBackToSystem(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-other-version", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-other-version")
	if p == nil {
		t.Fatal("pkg-other-version not found")
	}
	repo, _ := p["repo"].(string)
	if repo != "@System" {
		t.Errorf("pkg-other-version has no identical available — expected @System repo, got %q", repo)
	}
}

// ─── user-installed vs auto-installed ────────────────────────────────────────

// TestSearch_UserInstalled verifies that a package not listed in autoinst
// reports status=user-installed.
func TestSearch_UserInstalled(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-installed", "match": "exact", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-installed")
	if p == nil {
		t.Fatal("pkg-installed not found")
	}
	if p["status"] != "user-installed" {
		t.Errorf("expected status=user-installed for pkg-installed, got %q", p["status"])
	}
}

// TestSearch_AutoInstalled verifies that a package listed in autoinst
// reports status=auto-installed.
func TestSearch_AutoInstalled(t *testing.T) {
	tc := testcase(t, "tc-simple")
	result := invokeSimple(t, "search_packages",
		arg(map[string]string{"pattern": "pkg-auto-installed", "match": "exact", "testcase": tc}))
	pkgs := packages(t, result)
	p := findPackage(pkgs, "pkg-auto-installed")
	if p == nil {
		t.Fatal("pkg-auto-installed not found")
	}
	if p["status"] != "auto-installed" {
		t.Errorf("expected status=auto-installed for pkg-auto-installed, got %q", p["status"])
	}
}

// TestSearch_Details_DedupeOff verifies that details=true can return multiple
// rows for the same package name (one per repo/version).
func TestSearch_Details_DedupeOff(t *testing.T) {
	// tc-two-repos has versioned-pkg in two repos (1-0 and 2-0)
	tc := testcase(t, "tc-two-repos")
	result := invokeSimple(t, "search_packages",
		argAny(map[string]any{"pattern": "versioned-pkg", "match": "exact", "details": true, "testcase": tc}))
	pkgs := packages(t, result)
	if len(pkgs) < 2 {
		t.Errorf("expected >=2 rows for versioned-pkg with details=true (one per repo), got %d", len(pkgs))
	}
}

// TestSearch_Default_Deduped verifies that details=false returns exactly one
// row per package name even when the package exists in multiple repos.
func TestSearch_Default_Deduped(t *testing.T) {
	tc := testcase(t, "tc-two-repos")
	result := invokeSimple(t, "search_packages",
		argAny(map[string]any{"pattern": "versioned-pkg", "match": "exact", "testcase": tc}))
	pkgs := packages(t, result)
	count := 0
	for _, p := range pkgs {
		if p["name"] == "versioned-pkg" {
			count++
		}
	}
	if count != 1 {
		t.Errorf("expected exactly 1 row for versioned-pkg with details=false, got %d", count)
	}
}

// TestFindDependents_InstalledOnly_FiltersResults verifies that installed_only
// filters the dependent results — not the seed — so the seed is always found.
func TestFindDependents_InstalledOnly_FiltersResults(t *testing.T) {
	// pkg-dep exists in tc-deps; pkg-with-dep requires it but is not installed.
	// installed_only=true should return zero dependents, not a NOT_FOUND error.
	tc := testcase(t, "tc-deps")
	result := invokeSimple(t, "find_dependents",
		argAny(map[string]any{"package": "pkg-dep", "installed_only": true, "testcase": tc}))
	pkgs := packages(t, result)
	for _, p := range pkgs {
		status, _ := p["status"].(string)
		if status != "user-installed" && status != "auto-installed" &&
			status != "other-version-user" && status != "other-version-auto" {
			t.Errorf("installed_only=true returned non-installed dependent %v (status=%q)", p["name"], status)
		}
	}
}
