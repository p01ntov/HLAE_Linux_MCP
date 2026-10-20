# Tonworld subagents

Project client: `tools/tonworld_agents.py`; non-secret settings:
`tools/tonworld-agents.json`. Credentials remain in
`~/.config/tonworld/api-key`, checked and read privately by the installed Astra
skill. They are never copied into this project or passed in command arguments.
The user explicitly authorized the same key on both configured routers.

## Observed failures and mitigation, 2026-10-18

- Main router, Astra: HTTP 524 after 125.3 s, before any answer. This matches
  Cloudflare's default 125-second origin read timeout; increasing the client's
  timeout cannot remove a timeout imposed by the proxy. See
  [Cloudflare 524 documentation](https://developers.cloudflare.com/support/troubleshooting/http-status-codes/cloudflare-5xx-errors/error-524/).
- Main router, Sol: a separate client socket timeout after 600 s, no response
  headers. Earlier installed-client calls had an additional 180-second cap.
  These facts do not establish why the upstream was slow.
- Both routers accept Responses WebSocket handshake/ping, but generation
  probes closed with 1013, `no available account`. A successful handshake does
  not establish model availability on that transport. WebSocket is therefore
  not the default generation transport.
- Default now uses Chat Completions SSE on the user-supplied
  `https://kube943.prod.tonworld.ru/v1`, a 600 s socket/event timeout and a
  1200 s total job deadline. Tasks/context are capped locally at 8192 bytes;
  large assignments are divided into focused subtasks.
- Real work completed through this configuration: Astra xhigh camera-clock
  review (23.5 s), Astra camera diagnostic review (100.9 s), Sol 6.1 max floor
  validation helper (117.2 s). Returned models were
  `codex-native/gpt-6-astra` / `codex-native/gpt-6.1-sol`; all stopped normally,
  with `truncated: false`. This verifies bounded tasks, not unlimited payloads.
- The Sol response reported 4842 completion tokens, including 4140 reasoning
  tokens, despite `max_completion_tokens: 2048`. Treat that parameter as a
  provider-dependent hint, not a verified hard billing/computation bound.

No failed generation is automatically retried or silently submitted to another
router. Status includes endpoint, requested/response model, effort, time to
headers, first event/text, finish reason and usage. Only visible answer text is
saved; private reasoning deltas are ignored. HTTP/SSE provider error metadata
is bounded and key-redacted. Each job retains a snapshot of its configuration.

```sh
.venv/bin/python tools/tonworld_agents.py config
.venv/bin/python tools/tonworld_agents.py start --model gpt-6-astra --task-file brief.txt
.venv/bin/python tools/tonworld_agents.py start --model gpt-6.1-sol --task-file brief.txt
.venv/bin/python tools/tonworld_agents.py status JOB_ID
.venv/bin/python tools/tonworld_agents.py result JOB_ID
```

The helpers are external API assistants: they return text/code; the parent
applies changes and runs checks. Built-in Codex collaboration agents are not
used by this client.
