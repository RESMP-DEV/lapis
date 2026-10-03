## Preflight result

No files were edited. Inspection is at `efb17dbe85d20d6759e15b0ccf3f5634e00ba188`; it is one documentation-only commit above main `4750360a38569146eba14fc478f36ea499d8ec3e`. The eventual candidate must record its own revision, artifact digest, dirty flag, OS, CLI versions, commands, and limits. `sindexer` could not run because the read-only sandbox denied its temporary-file creation; I used bounded grep and direct reads instead.

## Reusable evidence

| Evidence | Revision | Scope and reuse disposition |
| --- | --- | --- |
| `build/reports/quality/receipt.json` | `4750360a`, clean | Non-GUI quality, Ruff, format, and Python unit discovery. Reusable through the current docs-only head under the result-reuse rule, but not after the candidate merge. Do not rerun equivalent Python unit cases separately before then. |
| `evidence/pr43-paste-admission.json` | `9258f74ad646b1794f1eb1e8a35e0ebc8684aaf5` | PR 43 paste-admission contract: real PTY, queue refusal, mode-dependent encoding, correlated receipt, sanitizers, and background UI. Strong design/source evidence, but not a current-source pass: `session_service.cpp`, `live_connection.cpp`, `terminal_surface.cpp`, tests, and harness have changed. |
| `evidence/pr71-idle-paste-admission.json` | `a7efb95cd77a23db2b1791e92d8ebc8d5a05b218` | Real service and managed Claude hook fixture for idle submit and permission refusal. Not a current-source pass: service and harness changed after it. |
| `evidence/presentation-review-followup.json` | `59fe24d1c760cba07aae1467f7f3461cafe46f8b` | Presented-frame ownership, real paste-request assertions, reset publication, and background/sanitizer scope. Not a current-source pass. |
| `evidence/accounts-review-followup.json` | `1491b90bb263f7f940662dee60319e385c25e167` | Account/reset diagnostic and launch-policy coverage; explicitly no live provider, remote credential transfer, or Codex desktop launch. Not a current-source pass. |
| `evidence/limit-reset-review-repairs.json` | `6d6ed155bb7b36246ea35f7c1fb5f21257ace08d` | Provider stand-ins, journal identity, no-replay, and callback publication. No live provider operation. Not a current-source pass. |
| `evidence/plan-sign-in-review-repairs.json` | `bf50593729a4057566f671ad591a35a58fce9e80` | Local account/session handling and credential staging with private fixtures; no OAuth, remote host, or provider call. Not a current-source pass. |

No tracked receipt was found for the current remote-account refusal implementation introduced around `8922564` and hardened in `28ec607`. The source tests exist, but a missing historical receipt is not a fresh pass.

## R2 input-integrity candidate gate

| Contract | Assembled-candidate case | Exact oracle |
| --- | --- | --- |
| Core wire, identity, PTY lifetime | Selected CTest suites | `local-protocol`, `live-connection`, `terminal-behavior`, and `pty-process` pass. |
| Full queue, maximum paste, mode change, correlated receipt, joined peer, partial frame | CLI case `paste-admission | Actual PTY digest matches; full queue rejects whole; bracketed markers appear only in service encoding; stale/partial peers do not corrupt input. |
| Slow reader, non-reader, output retention, child survival | CLI case `backpressure | Input queue overflow reports and preserves the child; output drains in order; a non-reader is bounded and the child remains recoverable. |
| Claude idle submit and pending-permission refusal | CLI case `claude-idle-paste-admission` | Idle observation-only notice may paste-plus-Return; permission request refuses without replay. |
| Stale attachment, replacement, malformed input | CLI cases `identity-boundaries` and `mismatch` | Stale generation is rejected; replacement does not reuse identity; malformed attachment preserves the active client. |
| Side-shell service behavior | CLI case `literal-resize-exit` | Literal argv/cwd, resize, paste echo, and exit status survive. |
| Qt focus, IME, ownership, refusal, disconnect, side terminal | `python3 scripts/lapis.py ui-review --json` | Background UI, shortcuts, and terminal-input fixtures pass. The side terminal receives typed input and returns keyboard ownership. Refused/lost paste reports uncertainty without replay. |
| macOS AppKit input, system pasteboard, Japanese IME | Parent-owned `just native-input` | Exact native bytes and bracket markers; composition and focus transfers do not split or misroute paste. |
| Installed Codex TUI input path | Parent-owned selected CLI case `codex-terminal` | Installed binary exercises navigation, paste, resize, reattach, and interrupt without a prompt. |
| Memory/races | Focused ASan/UBSan and TSan selections | Same contracts pass in separately instrumented builds; neither result substitutes for the other. |

## R3 account-identity candidate gate

| Contract | Assembled-candidate case | Exact oracle |
| --- | --- | --- |
| Plan-name and remote refusal preflight | `lapis_workspace_tests --case remote-accounts` | Unsafe plan names fail parsing; missing selected Claude/Codex credentials refuse before launch/replacement; malformed remote Claude tokens refuse and the healthy listener is not contacted. |
| Saved remote preamble without configured plans | `lapis_workspace_tests --case remote-account-reset` | Registry restart clears the stale visiting plan and shell preamble instead of using machine sign-in. |
| Local selection, own-sign-in isolation, local refusal while healthy | `lapis_workspace_tests --case accounts` | Own sign-in has no visiting override; load selection follows the configured pool; hidden local Claude credential refuses the switch while the existing session remains ready. |
| Codex home preparation | `lapis_accounts_tests` | A missing/unusable shared home or blocked destination refuses before child start and preserves the selected account home. |
| Reset identity, durable admission, no replay | `lapis_limit_resets_tests` | Provider stand-ins prove prepare/consume separation, account identity locks, lost reply uncertainty, restart reconciliation, malformed journal refusal, provider refusal handling, and alias sharing without a second consume. |
| Script-level account/provider fields and missing visiting credentials | Included once in assembled `just quality` | Existing Python tests prove malformed fields remain unknown, visiting credentials do not query the provider, and explicit refusals do not masquerade as unknown. |
| Real selected account, read-only identity | Parent-owned helper invocation below | Claude and Codex report the configured email and provider identity; the JSON contains no spend/confirmation and no reset call is made. |
| Healthy remote transport survives refusal | Missing candidate-owned case | A real attached PTY remains ready with the same child after `applyAccount` refuses; no reload, replacement, or preamble mutation occurs. |
| Unreadable and malformed local credentials | Missing focused cases | Oversized Claude token, malformed/oversized Codex `auth.json`, and permission-denied credentials fail before child birth/replacement. |
| Real remote route with managed credential | Parent-owned only | On each advertised remote route, the selected plan reports the expected identity without credential transfer or provider spend. |

## Exact candidate commands

Run serially in one candidate checkout and build owner. Replace `<candidate>` with the candidate receipt mode.

First establish the common gate once:

```sh
python3 scripts/lapis.py quality
python3 scripts/lapis.py build
```

Run the focused compiled source contracts once:

```sh
ctest --test-dir build/desktop -R '^(local-protocol|live-connection|terminal-behavior|pty-process)$' --output-on-failure --no-tests=error
build/desktop/apps/desktop/lapis_workspace_tests --case remote-accounts
build/desktop/apps/desktop/lapis_workspace_tests --case remote-account-reset
build/desktop/apps/desktop/lapis_workspace_tests --case accounts
build/desktop/apps/desktop/lapis_accounts_tests
build/desktop/apps/desktop/lapis_limit_resets_tests
build/desktop/apps/desktop/lapis_plan_sign_in_tests
```

Run the assembled real-service selection once:

```sh
python3 scripts/lapis.py cli-check \
  --case paste-admission \
  --case backpressure \
  --case identity-boundaries \
  --case synchronization-boundaries \
  --case claude-idle-paste-admission \
  --case literal-resize-exit \
  --case mismatch \
  --output build/reports/r2-candidate/cli-launch.json
python3 scripts/lapis.py ui-review --json
```

Run focused sanitizer evidence separately. Use the documented desktop-enabled configuration for each preset. For each of `asan` and `tsan`, with `build/desktop-asan` or `build/desktop-tsan`, run:

```sh
cmake --build build/desktop-asan --parallel 8
ctest --test-dir build/desktop-asan -R '^(local-protocol|live-connection|terminal-behavior|pty-process)$' --output-on-failure --no-tests=error
QT_QPA_PLATFORM=offscreen build/desktop-asan/apps/desktop/lapis_terminal_input_tests --background
QT_QPA_PLATFORM=offscreen build/desktop-asan/apps/desktop/lapis_workspace_tests --case remote-accounts
QT_QPA_PLATFORM=offscreen build/desktop-asan/apps/desktop/lapis_workspace_tests --case remote-account-reset
ASAN_OPTIONS=detect_leaks=0 build/desktop-asan/apps/desktop/lapis_limit_resets_tests
```

Repeat the same commands with `desktop-tsan`; omit `ASAN_OPTIONS` there. Keep the two builds separate.

Parent-owned native/provider checks must not run in a worker or concurrent with other GUI checks:

```sh
just native-input
python3 scripts/lapis.py cli-check --case codex-terminal --codex \
  --output build/reports/r2-candidate/codex-terminal.json
```

For each advertised selected local account, run only the read-only helper shape, with values supplied privately by the parent:

```sh
uv run --no-project python apps/desktop/src/limit_resets.py \
  --claude-plan "$CLAUDE_PLAN" \
  --claude-token-file "$CLAUDE_TOKEN_PATH" \
  --expected-email "$EXPECTED_EMAIL" \
  | tee build/reports/r3-candidate/claude-identity.json
```

```sh
uv run --no-project python apps/desktop/src/limit_resets.py \
  --codex-plan "$CODEX_PLAN" \
  --codex-home "$CODEX_ACCOUNT_HOME" \
  --expected-email "$EXPECTED_EMAIL" \
  --expected-account-id "$EXPECTED_ACCOUNT_ID" \
  | tee build/reports/r3-candidate/codex-identity.json
```

Acceptance requires `.phase == "final"`, the expected reported identity, no `.result`, no `.confirmed`, and no spend/operation fields. Do not add `--apply`, `--now`, `--prepare`, or `--reconcile-only` to this identity check.

## Missing discriminators before candidate sign-off

R2 lacks one real-service case where the client disconnects after a paste request but before its receipt and the candidate proves the PTY saw either the exact whole operation or a correlated uncertainty, with no replay. It also lacks a product side-shell paste case matching the actual PTY bytes and bracket markers; existing background coverage types and closes the side shell but does not paste into it. Native macOS presentation and package acceptance remain separate gates.

R3 lacks local selected-plan tests for unreadable credentials, an oversized Claude token, and malformed or oversized Codex `auth.json`; current local Codex preflight checks existence/readability but not JSON validity or the 1 MiB bound. It also lacks the explicitly parent-owned healthy real remote transport refusal. No current test proves the actual provider-selected account on every advertised route.

## Duplicate-run boundaries

Do not rerun `just cli-check` after the selected CLI command above; that command already is the selected CLI harness. Do not run `terminal-input` or `ui-preview` manually after `ui-review` passes. Do not run R3 Python unit modules separately after assembled `just quality`; rerun them only if quality fails or the candidate source changes. Do not run normal CTest twice; sanitizer builds are separate evidence, not retries. Do not run native input, installed Codex, or two GPU/native checks concurrently.

## Safety boundaries and non-claims

This preflight performed no credential access, provider request, reset spend, sign-in, SSH operation, GUI launch, or build. It made no repository edits. Candidate commands may write only ignored build/report paths and must use disposable fixture homes and sockets. Provider identity commands must never print token bodies, auth JSON, command-line secrets, or unsanitized provider identifiers.

A passing R2/R3 selection will not prove R1 process lifetime, R4 package/source binding, update recovery, real reboot, sustained soak, GPU presentation, distribution signing, or provider idempotency. Reset consumption is not required for the local-only candidate. The historical receipts remain dated observations, not current-head or candidate qualification.