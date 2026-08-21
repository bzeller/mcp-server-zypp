// Unit tests for progressMessage — the pure, side-effect-free core of
// handleProgress. Extracted specifically so this logic is testable without
// faking an *mcp.CallToolRequest with a live session.
package tools

import "testing"

func intp(v int) *int { return &v }

func TestProgressMessage_InstallProgress(t *testing.T) {
	msg, percent, total, ok := progressMessage(progressFrame{
		Action: "install", Package: "foo", Percent: intp(42),
	})
	if !ok {
		t.Fatal("expected ok=true")
	}
	if msg != "install foo" {
		t.Errorf("message = %q", msg)
	}
	if percent != 42 || total != 100 {
		t.Errorf("percent/total = %v/%v, want 42/100", percent, total)
	}
}

func TestProgressMessage_InstallFinished(t *testing.T) {
	msg, percent, _, ok := progressMessage(progressFrame{
		Action: "install", Package: "foo", Finished: true,
	})
	if !ok || msg != "install finished" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
	if percent != 100 {
		t.Errorf("percent = %v, want 100 on finish with no explicit Percent", percent)
	}
}

func TestProgressMessage_InstallFailed(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{
		Action: "install", Finished: true, Error: true,
	})
	if !ok || msg != "install failed" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
}

func TestProgressMessage_DownloadCached(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{
		Action: "download", Package: "foo", Cached: true,
	})
	if !ok || msg != "download foo (already cached)" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
}

func TestProgressMessage_CleanupUsesGenericPackageBranch(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{
		Action: "cleanup", Package: "foo-1.0-1.x86_64",
	})
	if !ok || msg != "cleanup foo-1.0-1.x86_64" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
}

func TestProgressMessage_RpmOutputTakesPriorityOverAction(t *testing.T) {
	msg, percent, total, ok := progressMessage(progressFrame{
		Action: "script", ScriptType: "posttrans", RpmOutput: "some raw output",
	})
	if !ok {
		t.Fatal("expected ok=true")
	}
	if msg != "some raw output" {
		t.Errorf("message = %q, want the raw output verbatim", msg)
	}
	if percent != 0 || total != 0 {
		t.Errorf("percent/total = %v/%v, want 0/0 for a log-line frame", percent, total)
	}
}

func TestProgressMessage_RpmLogLevelsUseLibzyppPrefixes(t *testing.T) {
	cases := []struct {
		level string
		want  string
	}{
		{"critical", "fatal error: boom"},
		{"error", "error: boom"},
		{"warning", "warning: boom"},
		{"info", "boom"},
		{"debug", "D: boom"},
	}
	for _, c := range cases {
		msg, percent, total, ok := progressMessage(progressFrame{
			Action: "rpm_log", Level: c.level, Line: "boom",
		})
		if !ok {
			t.Fatalf("level %q: expected ok=true", c.level)
		}
		if msg != c.want {
			t.Errorf("level %q: message = %q, want %q", c.level, msg, c.want)
		}
		if percent != 0 || total != 0 {
			t.Errorf("level %q: percent/total = %v/%v, want 0/0", c.level, percent, total)
		}
	}
}

func TestProgressMessage_RpmLogWithoutLineSuppressed(t *testing.T) {
	_, _, _, ok := progressMessage(progressFrame{Action: "rpm_log", Level: "info"})
	if ok {
		t.Error("expected ok=false when rpm_log frame has no line")
	}
}

func TestProgressMessage_ScriptRunning(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{
		Action: "script", ScriptType: "posttrans", Package: "foo",
	})
	if !ok || msg != "Executing posttrans script: foo" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
}

func TestProgressMessage_ScriptFinishedSeverities(t *testing.T) {
	cases := []struct {
		severity string
		want     string
	}{
		{"none", "posttrans script finished"},
		{"warning", "posttrans script warning"},
		{"critical", "posttrans script failed"},
	}
	for _, c := range cases {
		msg, _, _, ok := progressMessage(progressFrame{
			Action: "script", ScriptType: "posttrans", Finished: true, Severity: c.severity,
		})
		if !ok || msg != c.want {
			t.Errorf("severity %q: got msg=%q ok=%v, want %q", c.severity, msg, ok, c.want)
		}
	}
}

func TestProgressMessage_TransactionPhase(t *testing.T) {
	msg, percent, _, ok := progressMessage(progressFrame{
		Action: "transaction", Name: "Verifying", Percent: intp(50),
	})
	if !ok || msg != "Verifying" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
	if percent != 50 {
		t.Errorf("percent = %v, want 50", percent)
	}
}

func TestProgressMessage_TransactionFinishedFailed(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{
		Action: "transaction", Name: "Verifying", Finished: true, Error: true,
	})
	if !ok || msg != "Verifying failed" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
}

func TestProgressMessage_TransactionNameFallsBackToAction(t *testing.T) {
	msg, _, _, ok := progressMessage(progressFrame{Action: "transaction"})
	if !ok || msg != "transaction" {
		t.Errorf("got msg=%q ok=%v, want action used as fallback name", msg, ok)
	}
}

func TestProgressMessage_UnknownActionNoPackage(t *testing.T) {
	msg, percent, _, ok := progressMessage(progressFrame{Action: "preload"})
	if !ok || msg != "preload" {
		t.Errorf("got msg=%q ok=%v", msg, ok)
	}
	if percent != 0 {
		t.Errorf("percent = %v, want 0", percent)
	}
}
