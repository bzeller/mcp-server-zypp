"""
MCP-over-stdio client used by every testsuite scenario — replaces the old
e2e suite's FrameClient (which drove zypp-mcp-tool directly, bypassing the
Go proxy entirely). Talks to the real, installed mcp-server-zypp binary
over the actual MCP protocol: initialize, tools/call, elicitation,
progress notifications. See .opencode/plans/testsuite-package.md for the
full design rationale.

Deliberately one anyio.run() per call_tool() invocation, spinning up a
fresh subprocess/session each time — this mirrors the old FrameClient's
"one process, one call" model exactly (each call got its own
ZYPP_LOGFILE), so scenario bodies written against the old transport need
no changes beyond the import.

Note what this client does NOT need to handle, compared to the old
FrameClient: the "zypp_control"/"commit_active" cancellation-barrier
handshake (worker/src/callbacks.cc) is acked entirely inside the proxy
(proxy/internal/worker/worker.go) and never reaches the MCP client at
all — there is nothing to answer here.
"""
import json
import os
import shlex
import sys

import anyio
from mcp import ClientSession
from mcp.client.stdio import StdioServerParameters, stdio_client
from mcp.types import ElicitResult


def _server_command():
    """The server binary + args to run, from $MCPSERVER/$MCPSERVER_ARGS —
    defaulting to the real installed binary. $MCPSERVER_ARGS exists solely
    so a developer's run-in-container.py can pass -worker-dir for an
    uninstalled build tree, without this shipped code knowing anything
    about build layouts."""
    server = os.environ.get("MCPSERVER", "/usr/bin/mcp-server-zypp")
    args = shlex.split(os.environ.get("MCPSERVER_ARGS", ""))
    return server, args


def _make_elicitation_callback(elicitation_answers):
    """Builds the elicitation_callback ClientSession expects.

    The worker emits a structured {method, data} elicitation frame (see
    worker/src/callbacks.cc), but the proxy flattens both into one display
    string before forwarding it to the MCP client:
        Message: fmt.Sprintf("[%s] %s", envelope.Method, string(envelope.Data))
    (proxy/internal/tools/tools.go). So the method name has to be parsed
    back out of the "[method] ..." prefix here — there is no structured
    field left to read it from. This is a real gap in the proxy's
    elicitation forwarding, flagged for a future fix; out of scope here.
    """
    async def _on_elicit(context, params):
        method = params.message.split("]", 1)[0].lstrip("[")
        print(f"[elicitation] method={method!r} message={params.message!r}",
              file=sys.stderr)
        if method not in elicitation_answers:
            # Mirrors the old FrameClient's close_stdin() behaviour: a
            # client that never answers is simulated by declining, not by
            # actually severing the connection (MCP has no equivalent of
            # "close stdin" for one in-flight request).
            return ElicitResult(action="decline")
        return ElicitResult(action="accept",
                            content={"answer": elicitation_answers[method]})
    return _on_elicit


def call_tool(tool: str, args: dict, elicitation_answers=None,
              log_tag: str = "call", progress_sink=None, extra_env=None) -> dict:
    """Invokes an mcp-server-zypp tool over real MCP-over-stdio and
    returns the worker's parsed JSON frame — same signature and same
    return shape as the old e2e suite's call_tool(), so every existing
    scenario assertion (result["code"], result["details"], ...) keeps
    working unchanged.

    elicitation_answers: see _make_elicitation_callback().
    progress_sink: if given, a list appended with one
        {"progress": ..., "total": ..., "message": ...} dict per
        notifications/progress the session receives during the call.
        This is the MCP-level shape the proxy derives from the worker's
        raw progress frame (proxy/internal/tools/tools.go:
        progressMessage()), NOT the worker's own frame shape — there is
        no "action"/"percent" key here, only a human-readable message
        string built from the worker's action (e.g. "install <pkg>",
        "preload: <file>", "preload finished"). Scenarios that need to
        distinguish frame kinds must match on message content.
    extra_env: merged into the *proxy's* environment. exec.Command in Go
        inherits the parent's environment when .Env is left unset (which
        proxy/internal/worker/worker.go does), so this reaches the worker
        subprocess unchanged — e.g. {"ZYPP_PCK_PRELOAD": "0"} to pin the
        commit download backend.
    log_tag: kept for call-site symmetry with the old transport; no
        per-call ZYPP_LOGFILE is arranged here since the worker is no
        longer spawned directly by test code — its own libzypp trace, if
        needed, must come from the server's own logging setup.
    """
    elicitation_answers = elicitation_answers or {}
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)

    server, extra_args = _server_command()

    async def _run():
        params = StdioServerParameters(command=server, args=extra_args, env=env)
        async with stdio_client(params) as (read, write):
            async with ClientSession(
                read, write,
                elicitation_callback=_make_elicitation_callback(elicitation_answers),
            ) as session:
                await session.initialize()

                async def _on_progress(progress, total, message):
                    if progress_sink is not None:
                        progress_sink.append(
                            {"progress": progress, "total": total, "message": message})

                result = await session.call_tool(
                    tool, args, progress_callback=_on_progress)

                if not result.content:
                    sys.exit(f"FAIL: {tool}: empty result content")
                text = result.content[0].text
                frame = json.loads(text)
                # The parsed frame's own "type" field (result/error) is
                # preferred over result.isError everywhere else in this
                # testsuite, so scenarios stay transport-agnostic — but
                # cross-check them here once, since a mismatch would mean
                # one of our two shape assumptions (§4.2) is wrong.
                if result.isError and frame.get("type") != "error":
                    print(f"[mcpclient] warning: isError=True but frame type "
                          f"is {frame.get('type')!r}", file=sys.stderr)
                return frame

    return anyio.run(_run)
