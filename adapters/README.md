# Agent adapters

Every CLI has a versioned adapter and an evidence-backed capability declaration.
Terminal byte transport remains generic; semantic attention is tool-specific.

Integration levels, strongest first:

1. A supported structured protocol, with explicit requests and response routing.
2. Native hooks that report lifecycle and attention events to local IPC.
3. A launcher wrapper that supplies a stable session identity and observes exits.
4. Terminal bells or heuristics as advisory signals only.

A launcher does not inherently know when a tool is awaiting approval. A regex
matching a prompt cannot be treated as an authoritative approval request.
Declare observation and response capabilities separately.

Hook bridges should use a session-scoped local endpoint and binding established
by the launcher. Keep hooks bounded and nonblocking; attention hooks should not
change the tool's execution/approval policy. Probe return-value semantics before
installing a hook because some tools treat hook output as a decision.

Each adapter must document its tested binary/version, transport, event mapping,
response ownership, disconnect semantics, and unsupported features. Credentials
and raw transcripts do not belong in adapter fixtures or shared receipts.

Codex is first. Other CLI tools require independent runtime investigation; no
Claude Code, Gemini CLI, or other hook compatibility is assumed by this scaffold.
