# Pane Activity and Rich Tab Status

Author: kaitao@microsoft.com

Date: 2026-08-21

Status: Proposed

## Summary

Windows Terminal tabs expose the title and icon of their active pane, but they
do not provide a general way to understand what is happening in the rest of
the tab. This is increasingly limiting when a tab contains multiple shell
panes, a stashed Agent Pane, or long-running work that completes while the user
is looking elsewhere.

This specification introduces a normalized, per-pane activity model. Shell
integration, applications, agents, and Terminal itself report semantic
signals. Terminal reduces those signals into authoritative `PaneActivityState`
instances, aggregates them into a `TabActivityState`, and projects the most
important result into a compact, single-line tab indicator.

The core principle is:

> Producers report facts. Terminal owns state and attention policy. Tabs
> aggregate panes. UI renders the result.

The state-model milestone reuses existing protocols and internal events. It
does not introduce a new public OSC sequence, a provider framework, or a
second tab-header line. Activity Center V1 adds a window-local overview over
that state without changing the horizontal tab layout.

## Problem

With one pane, the tab title can provide a useful description of the foreground
application. With multiple panes, the title still represents only the active
pane. Important activity in another split or a stashed Agent Pane is invisible
until the user finds and focuses it.

Existing signals solve individual parts of the problem:

- OSC 0/2 changes the pane title.
- OSC 9;4 reports application progress.
- BEL raises a tab bell indicator.
- OSC 133 marks shell prompt, command, output, and completion boundaries.
- WTA knows when an agent is working, waiting for permission, completed, or
  failed.
- Terminal knows pane visibility, focus, output, and connection state.

These signals do not currently converge on one state model. The UI therefore
cannot consistently answer:

- Is work still running?
- Is something waiting for the user?
- Did background work complete or fail?
- Which pane caused the status?
- Has the user actually seen it?

## Prior art

This design unifies several long-running Windows Terminal scenarios rather than
introducing an unrelated Rich Tab concept:

- [microsoft/terminal#1620](https://github.com/microsoft/terminal/issues/1620)
  requests a tab busy indicator for output or bell activity.
- [microsoft/terminal#7955](https://github.com/microsoft/terminal/issues/7955)
  discusses inactive-output indicators, next-prompt notifications, and using
  OSC 133 command boundaries to detect running and completed commands.
- [microsoft/terminal#6372](https://github.com/microsoft/terminal/issues/6372)
  describes completion notification as a shift from repeatedly checking work
  to being notified when it finishes.
- [microsoft/terminal#9481](https://github.com/microsoft/terminal/issues/9481)
  requests visible tab error state in addition to progress.
- [microsoft/terminal#10090](https://github.com/microsoft/terminal/issues/10090)
  established prior art for aggregating progress across panes.
- [microsoft/terminal#19788](https://github.com/microsoft/terminal/issues/19788)
  extends attention discovery to navigation across tabs, panes, and windows.

The existing
[Shell Integration (Marks)](./%2311000%20-%20Marks/Shell-Integration-Marks.md)
spec defines the OSC 133 prompt, command, output, and completion boundaries.
This specification consumes those semantic boundaries as live activity signals
without changing their buffer-mark behavior.

## Goals

- Define one standard activity contract for every pane type.
- Preserve the existing active-pane title and icon behavior.
- Surface important activity from non-active and stashed panes on the tab.
- Support both single-pane and multi-pane tabs.
- Make every tab indicator traceable to the pane that produced it.
- Keep protocol parsing, state reduction, aggregation, and presentation
  separate.
- Reuse existing OSC and WTA signals before adding a new wire protocol.
- Preserve enough source and revision information to diagnose stale or
  conflicting state.

## Non-goals

- Replacing the tab title with an activity summary.
- Adding a second line to the tab header.
- Parsing visible terminal text to infer success, failure, or percentage.
- Displaying full command lines by default.
- Tracking arbitrary background jobs that the shell itself cannot identify.
- Providing historical activity storage.
- Designing a general third-party UI provider system.
- Adding a cross-window activity center in V1.
- Allowing an activity signal to execute terminal actions or inject input.

## Terminology

- **Signal**: An immutable semantic fact received from a producer, such as
  `OperationStarted` or `ProgressChanged`.
- **Reducer**: Terminal-owned logic that applies signals to a pane's current
  activity state.
- **Operation**: One foreground unit of work, such as a shell command, an agent
  turn, or an application task.
- **Attention**: Terminal-owned state indicating that the user has not seen or
  resolved an important condition.
- **Observed**: The source pane was actually visible in a foreground window
  and was the active focused pane when the relevant event occurred, or the
  user subsequently focused that exact pane. Selecting a tab does not observe
  its visible sibling panes.
- **Dominant pane**: The pane whose state currently determines the tab
  indicator.

## Design principles

### Pane state is authoritative; tab state is derived

Work happens in panes. A tab does not own an independent execution state. It
aggregates the states of the logical panes that belong to it.

### State and attention are orthogonal

A pane can be working without requiring attention, or idle after a failed
operation while still requiring attention. `Working`, `Waiting`, `Update`, and
`Error` must not be forced into one mutually exclusive enum.

### Structured signals beat output heuristics

The string `Build failed` may be historical output, a test fixture, or a log
message. Raw output can establish that bytes arrived; it cannot establish
command lifecycle or outcome. Semantic state comes from OSC, typed agent
events, or Terminal-owned lifecycle events.

### Wire producers do not control UI

No producer can request a red tab, choose an icon, or set aggregation priority.
It reports a semantic event. Terminal maps that event to presentation according
to product policy and accessibility requirements.

## Architecture

```text
ConPTY output
  -> VT parser
  -> OSC adapter -------------------+
                                     |
WTA / ACP events                     |
  -> Agent adapter ------------------+-> PaneActivitySignal bus
                                     |           |
Connection, focus, and output events |           v
  -> Terminal adapter ---------------+   per-pane reducer
                                                 |
                                                 v
                                         PaneActivityStore
                                                 |
                                                 v
                                         tab aggregator
                                                 |
                                                 v
                                  tab indicator / tooltip / navigation
```

The activity store belongs in TerminalApp rather than TerminalCore. TerminalCore
continues to parse VT and maintain buffer marks, but Agent Pane and future
non-terminal content do not necessarily own a TerminalCore.

Signals for a pane are serialized before entering its reducer. UI objects
consume immutable state snapshots or property-change notifications; they do not
interpret OSC or agent-specific payloads.

## Standard signal contract

The examples below are conceptual C++ types. The implementation may use WinRT
types at UI boundaries, but the reducer should remain independently testable.

```cpp
enum class PaneActivitySignalKind
{
    PromptStarted,
    InputStarted,
    OperationStarted,
    ProgressChanged,
    WaitingForUser,
    OperationResumed,
    OperationFinished,
    AttentionRequested,
    ContextChanged,
    ConnectionChanged,
    OutputObserved,
};

enum class PaneActivitySource
{
    ShellIntegration,
    Application,
    Agent,
    Terminal,
};

struct PaneActivitySignal
{
    PaneId paneId;
    PaneActivitySource source;
    PaneActivitySignalKind kind;
    std::optional<OperationId> operationId;
    PaneActivitySignalPayload payload;
    uint64_t revision;
    std::chrono::steady_clock::time_point receivedAt;
};
```

The adapter, not an in-band producer, supplies `paneId`, `revision`, and
`receivedAt`. An OSC sequence can only affect the pane whose output stream
contains it. Producer timestamps can be retained as metadata, but ordering uses
Terminal receipt order.

`operationId` is optional at an input boundary. For V1 shell integration,
Terminal creates an ID on `OSC 133;C` and associates the next `OSC 133;D` with
the active foreground shell operation. Agent events should retain their
existing turn or session identifiers where available.

## Input adapters

### Shell integration

Shell integration installs small callbacks into the shell's interactive loop.
It does not infer lifecycle by scraping output.

| Sequence | Normalized signal | Meaning |
| --- | --- | --- |
| `OSC 133;A` | `PromptStarted` | A prompt is starting |
| `OSC 133;B` | `InputStarted` | The prompt ended and command input started |
| `OSC 133;C` | `OperationStarted` | Input ended and command execution/output started |
| `OSC 133;D;<code>` | `OperationFinished` | The foreground command ended |
| `OSC 9;9;<cwd>` | `ContextChanged` | The current working directory changed |
| `OSC 9001;ShellType;...` | `ContextChanged` | Shell identity changed |

`OSC 133;D` carries an exit code, not a UI severity. V1 maps zero to
`Succeeded` and nonzero or unparsable values to `Failed`. The original code is
retained with the operation result. At completion, Terminal also reads the
command text from the matching shell-integration mark; it does not infer the
command from visible output or an unrelated history entry. A nonzero exit code
is useful evidence, but it is not a universal statement that the user considers
the command erroneous.

The current Intelligent Terminal PowerShell and Bash integrations emit
`D/A/B`, CWD, and shell identity. They do not currently emit `OSC 133;C`.
Consequently, they can identify completion and result but cannot precisely
distinguish command editing from command execution. Reliable shell `Working`
state requires extending each integration at its command-submission boundary:

- PowerShell can wrap the command-reading boundary so it emits `C` after the
  user submits a line and before PowerShell executes it.
- Bash requires a guarded pre-execution hook because its generic debug callback
  also runs for integration-internal commands.
- Zsh provides a direct pre-execution callback.

The implementation must preserve user prompt functions, key bindings, and
existing shell hooks. If reliable `C` emission cannot be installed, the pane
remains `Unknown` or `Idle`; Terminal must not pretend that `B` means
`Working`.

### Applications

Any process writing to the pane can emit supported control sequences. This path
does not depend on shell integration.

| Input | Normalized signal |
| --- | --- |
| OSC 0/2 title | `ContextChanged(title)` |
| OSC 9;4 clear/set/error/indeterminate/paused | `ProgressChanged` |
| BEL | `AttentionRequested` |

Application progress attaches to the current foreground operation when one
exists. If no operation exists, non-clear progress creates an application
operation contribution. Clearing progress removes that contribution.

Raw output produces a coalesced `OutputObserved` signal and updates
`lastOutputAt`. V1 does not turn arbitrary output into `Working`, `Error`, or
an unread tab indicator. A future `notifyOnInactiveOutput` policy can consume
the same signal without changing the state contract.

### Agent and WTA

Agent state uses internal typed events rather than encoding WTA lifecycle
through OSC.

| Agent event | Normalized signal |
| --- | --- |
| Turn started | `OperationStarted(kind=AgentTurn)` |
| Tool activity | Operation detail or progress update |
| Permission or user input requested | `WaitingForUser` |
| Permission supplied | `OperationResumed` |
| Turn completed | `OperationFinished(Succeeded)` |
| Turn failed | `OperationFinished(Failed)` |
| Turn cancelled | `OperationFinished(Cancelled)` |

Events must preserve pane, tab, and window ownership. The receiving adapter
validates that identity before updating the store. An Agent Pane may be
stashed, but its activity remains attached to its owning tab.

The Agent Pane identity is the WT Protocol pane ID: the terminal connection's
stable `SessionId` GUID, not `Pane::Id()`, which is a tab-local integer that may
be reassigned during a cross-window move. Terminal creates the GUID before the
Agent Pane connection starts, assigns it to `NewTerminalArgs.SessionId`, and
passes the same value to WTA as `--owner-pane-id`. WTA includes it in every
pane-local `agent_state_changed` and `agent_status` event:

```json
{
  "method": "agent_state_changed",
  "params": {
    "tab_id": "{tab-guid}",
    "pane_id": "00000000-0000-0000-0000-000000000000",
    "activity": {
      "phase": "working",
      "outcome": null,
      "operation_id": 17
    }
  }
}
```

The C++ receiver first resolves `tab_id`, then resolves `pane_id` within that
tab and verifies that the result hosts `AgentPaneContent`. Missing, invalid, or
mismatched identities are logged and rejected; the receiver must not fall back
to the first Agent Pane in the tab. The pane GUID survives stashing and
cross-window reattachment, while the existing `tab_renamed` event updates the
tab and window portions of the routing envelope.

Agent-reported ordinary tool calls are presentation evidence, not proof that
Terminal owns or can control those processes.

### Terminal-owned events

Terminal reports facts that no pane process can reliably supply:

- connection starting, ready, disconnected, and closed;
- pane focus and actual visibility;
- tab and window foreground state;
- pane stash, restore, move, detach, and close;
- coalesced output arrival.

Closing or detaching a pane removes it from the old tab's aggregation. Stashing
an Agent Pane does not.

## Pane activity state

```cpp
enum class PaneAvailability
{
    Connecting,
    Ready,
    Disconnected,
};

enum class PaneActivityPhase
{
    Unknown,
    Idle,
    Working,
    Waiting,
};

enum class PaneOperationOutcome
{
    Succeeded,
    Failed,
    Cancelled,
    Interrupted,
};

enum class PaneAttentionLevel
{
    None,
    Update,
    Error,
    ActionRequired,
};

struct PaneOperation
{
    OperationId id;
    PaneOperationKind kind;
    std::optional<std::string> summary;
    std::chrono::steady_clock::time_point startedAt;
    PaneActivitySourceSet evidence;
};

struct PaneOperationResult
{
    PaneOperationOutcome outcome;
    std::optional<uint32_t> exitCode;
    std::optional<std::string> commandPreview;
};

struct PaneActivityState
{
    PaneId paneId;
    PaneAvailability availability;
    PaneActivityPhase phase;
    std::optional<PaneOperation> operation;
    PaneProgress progress;
    std::optional<PaneOperationResult> lastResult;
    PaneContext context;
    uint64_t revision;
};

struct PaneAttentionState
{
    PaneAttentionLevel level;
    PaneAttentionReason reason;
    uint64_t raisedAtRevision;
    uint64_t acknowledgedAtRevision;
};
```

`PaneActivityState` describes facts. `PaneAttentionState` describes whether an
important fact is unresolved or unobserved. They are stored together per pane
but reduced separately.

`summary` is optional and bounded. A completed shell operation may retain a
separate, whitespace-normalized command preview of at most 240 characters for
the owning user's Activity Center. Agent-provided summaries must already be
suitable for compact display and must not contain credentials or full provider
configuration.

## Reducer rules

The reducer is deterministic and contains no XAML behavior.

| Signal | State transition |
| --- | --- |
| `PromptStarted` | Recover to `Idle`; interrupt any unmatched shell operation |
| `InputStarted` | Remain `Idle`; record the input boundary |
| `OperationStarted` | Set `Working`; create or update the operation |
| `ProgressChanged` | Update progress; create an application contribution if needed |
| `WaitingForUser` | Set `Waiting`; raise `ActionRequired` |
| `OperationResumed` | Set `Working`; resolve `ActionRequired` |
| `OperationFinished(Succeeded)` | Set `Idle`; save result; conditionally raise `Update` |
| `OperationFinished(Failed)` | Set `Idle`; save result; raise `Error` |
| `OperationFinished(Cancelled)` | Set `Idle`; save result |
| `ConnectionChanged(Disconnected)` | Set availability; interrupt work; raise `Error` |

Additional rules:

- `OSC 133;D` with no active shell operation records a boundary but does not
  fabricate an operation. This handles initial prompt setup and recovery.
- A new operation implicitly interrupts an unmatched previous operation from
  the same foreground source.
- `PromptStarted` is a recovery boundary. If a shell integration missed `D`,
  Terminal must not leave the pane permanently `Working`.
- Progress percentages are clamped at the input adapter. The reducer never
  estimates percentages from output volume.
- A connection failure overrides activity presentation, but the last operation
  and result remain available for diagnostics.
- Duplicate or stale out-of-band agent revisions are ignored. In-band OSC order
  follows the pane's byte stream.

## Attention and observation

Attention is a user-facing policy layered over factual state.

### Raising attention

- Successful completion raises `Update` only when the source pane was not
  observed at completion.
- BEL in an unobserved pane raises `Update`.
- `WaitingForUser` raises `ActionRequired` regardless of visibility.
- Failed completion and disconnection raise `Error` regardless of visibility.
- V1 has no minimum-duration threshold. Duration-based notification policy can
  be added without changing the signal or state schema.

A pane is observed when its content is visible in the foreground window at the
time of the event. Merely belonging to the selected tab is insufficient for a
stashed pane. A visible split may be considered observed even when it does not
have keyboard focus.

### Clearing attention

- `Update` clears when the source pane becomes observed after
  `raisedAtRevision`.
- `ActionRequired` clears only when the operation resumes, finishes, or is
  cancelled.
- `Error` can be acknowledged when the source pane is focused. The factual
  `lastResult` remains until the next operation supersedes it.
- `Working` is a phase, not attention, and never clears because the user
  focused the pane.

Selecting a multi-pane tab does not clear attention from every pane.

## Tab aggregation

Every logical pane still owned by the tab participates:

- the active pane;
- other visible split panes;
- zoomed but temporarily hidden sibling panes;
- a stashed Agent Pane.

Closed, detached, or moved panes do not participate.

```cpp
struct TabActivityState
{
    PaneId activePaneId;
    std::optional<PaneId> dominantPaneId;
    PaneActivityPhase phase;
    PaneAttentionLevel attention;
    uint32_t activePaneCount;
    uint32_t attentionPaneCount;
    std::optional<PaneProgress> progress;
    std::optional<std::string> summary;
    uint64_t revision;
};
```

### Identity

The active pane continues to determine the tab title and icon. This preserves
the existing user model and avoids unstable titles when background panes
change.

### Dominant attention

The tab selects one dominant pane using:

```text
ActionRequired > Error > Update > None
```

Within one level, the newest unacknowledged state wins. `ActionRequired` ranks
above `Error` because it represents blocked work the user can immediately
unblock. Counts preserve the fact that other panes also have activity.

### Derived phase

When no attention determines presentation:

```text
any Waiting -> Waiting
else any Working -> Working
else all Idle -> Idle
else Unknown
```

`activePaneCount` counts each pane once when it is non-idle or has attention.
It is not the tab's total pane count.

### Progress

- One contributing pane: display that pane's progress.
- Multiple contributing percentages: display indeterminate progress and a
  count; never average unrelated operations.
- Active-pane progress wins only when no other pane has dominant attention.
- Attention presentation overrides progress presentation.

This generalizes the existing taskbar-state aggregation rather than replacing
its wire support. The existing OSC 9;4 states remain valid inputs.

## Tab presentation

The tab remains one line:

```text
[icon] [title] [activity indicator] [close]
```

V1 presentation:

| Derived state | Indicator |
| --- | --- |
| Idle / no attention | None |
| Working | Animated progress ring |
| Update | Solid unread dot |
| ActionRequired | Warning/action glyph |
| Error / disconnected | Error glyph |

The indicator must not rely on color alone. Its automation name includes the
state, source pane, and count, for example:

```text
project-x, action required in Copilot, 2 active panes
```

Animations honor the system animation setting.

### Single-pane tab

The indicator directly represents the pane. No count is shown.

### Multi-pane tab

The indicator represents the dominant pane and displays the number of panes
that are active or need attention. A tooltip lists non-idle panes in dominance
order:

```text
Copilot       Waiting for permission
PowerShell    Running
```

Idle panes are omitted by default.

### Navigation

Activating the indicator:

1. selects the owning tab;
2. restores a stashed target pane if required;
3. focuses the dominant pane;
4. lets the normal observation policy acknowledge `Update`.

`ActionRequired` and factual failure do not disappear merely because the
indicator was activated.

### Activity Center V1

Activity Center is a window-local flyout opened from the activity button beside
the new-tab button. It does not add or depend on vertical tabs.

The flyout is rebuilt from the authoritative per-pane store whenever it opens.
It includes panes that are working, waiting, or carry unresolved
`Update`, `Error`, or `ActionRequired` attention. Idle panes without attention
are omitted. Entries are ordered by:

```text
ActionRequired > Error > Update > Working
```

The tab-row Activity Center entry remains available in the empty state and
always uses the monochrome Segoe Fluent `Recent` glyph. It changes only from
subdued to normal opacity when a deliverable entry exists. It does not use
color, a numeric badge, or a notification glyph.

Delivery is a projection over pane state, not a separate state store. The
currently active tab never contributes entries. The global
`activityCenterDelivery` setting accepts `attentionOnly` (the default) or
`allActivity`. `attentionOnly` delivers only `Error` and `ActionRequired`;
routine `Working` and completed `Update` states remain available to tab-local
status but do not enter Activity Center. `allActivity` includes every
non-idle/current-attention entry from inactive tabs.

Each tab assigns a monotonically increasing receive sequence to every pane
activity signal. When panes have the same attention and phase priority, the
most recently received signal wins the tab's dominant indicator. Per-pane
revisions are not compared across panes.

Each entry is a wide, compact two-line row containing:

- state and source in a fixed leading column;
- a semantic summary, falling back to the pane title;
- tab title, pane number, hidden-pane state, and current working directory in
  a fixed trailing context column; and
- a thin progress indicator only for determinate or paused application
  progress.

For normal Agent turns, WTA projects a whitespace-normalized, 160-character
preview of the submitted prompt as the activity summary. Synthesized autofix
prompts use a generic description instead of exposing diagnostic payloads.
When no source summary exists, Activity Center avoids generic text that merely
repeats the state. Action-required and error entries retain actionable fallback
text; ordinary working entries use the pane title.

Activating a card selects its tab, restores a hidden source pane when needed,
focuses that exact pane, and applies the normal observation rule. An empty
flyout displays a disabled empty state.

V1 intentionally does not retain activity history, merge activities across
windows, provide filtering, or replace the horizontal tab strip.

## Compatibility with existing tab status

`TerminalTabStatus` already carries connection, zoom, progress, bell, read-only,
and broadcast-input properties. The Pane Activity implementation should extend
or compose with this model rather than create a second uncoordinated tab-header
status surface.

Existing behavior remains:

- title and icon come from the active pane;
- OSC 9;4 continues to drive taskbar and tab progress;
- BEL continues to honor bell notification settings;
- connection-closed status remains available;
- tab progress combines leaf-pane taskbar states.

The activity reducer consumes the same semantic events so progress, bell, and
connection status cannot disagree with a parallel Rich Tab model.

## Future explicit activity protocol

V1 does not add a public OSC sequence. Existing standards cover shell
lifecycle, progress, title, CWD, and bell; WTA has a typed internal channel.

If real applications need to report waiting state, operation summaries, or
correlated application tasks, a future version may extend the existing OSC
9001 namespace:

```text
OSC 9001;PaneActivity;1;Begin;id=42;summary=Running%20tests ST
OSC 9001;PaneActivity;1;Wait;id=42;reason=permission ST
OSC 9001;PaneActivity;1;Resume;id=42 ST
OSC 9001;PaneActivity;1;End;id=42;outcome=failed;code=1 ST
```

Before standardization, that protocol requires:

- coordination with existing `ShellType` and `AgentEvent` OSC 9001 routes;
- versioning and unknown-field behavior;
- percent encoding and strict length limits;
- operation lifetime and replacement rules;
- documented privacy guidance for summaries;
- tests through ConPTY, SSH, WSL, tmux, and nested shells.

The future wire format maps into the same internal signals and does not change
the reducer or UI contract.

## Security, privacy, and reliability

- In-band signals are untrusted display input.
- An OSC sequence can update only its source pane.
- Activity protocols cannot send input, mutate another pane, or bypass
  confirmation-gated terminal actions.
- Text fields are length-bounded and sanitized before rendering.
- Shell command previews are read only from shell-integration marks, bounded,
  and displayed locally; they are not logged or sent to WTA. Environment
  variables, credentials, bearer capabilities, and provider configuration are
  not added by the activity protocol.
- High-frequency progress and output signals are coalesced before reaching the
  UI thread.
- Lost shell boundaries recover at the next prompt or connection transition.
- A malformed sequence is ignored or normalized to an unknown result; it must
  not crash the parser or leave an unbounded allocation.

## V1 implementation plan

### Phase 1: State foundation

- Add testable signal, pane state, attention state, and reducer types.
- Add a TerminalApp-owned store keyed by stable pane identity.
- Adapt connection, focus, visibility, title, progress, bell, and existing OSC
  133 events.
- Add reducer and recovery tests before adding UI.

### Phase 2: Reliable shell lifecycle

- Add reliable `OSC 133;C` emission to supported shell integrations.
- Preserve user prompt and input customizations.
- Validate success, failure, cancellation, parse error, nested shell, and
  initial-prompt behavior.

### Phase 3: Agent activity

- Normalize WTA turn, permission, completion, cancellation, and failure events.
- Preserve explicit pane, tab, and window routing.
- Keep stashed Agent Pane activity attached to the owning tab.

### Phase 4: Tab aggregation and indicator

- Aggregate all logical panes.
- Add one accessible activity accessory to the existing tab header.
- Support single-pane and multi-pane counts.
- Navigate from the indicator to the dominant pane.

### Phase 5: Evaluation

- Measure false `Working`, stale attention, and noisy completion indicators.
- Evaluate whether successful completion needs a duration threshold.
- Decide whether a public explicit activity OSC is justified.
- Use the validated state model for a future Activity Overview.

## Test plan

### Reducer tests

- Normal `A/B/C/D;0` command lifecycle.
- Nonzero, missing, negative, and malformed completion codes.
- Initial `D` without a matching operation.
- Missing `D` recovered by the next `A`.
- Duplicate starts and out-of-order agent revisions.
- Waiting, resume, cancellation, and disconnect transitions.
- Progress with and without a current operation.

### Aggregation tests

- Single idle, working, completed, waiting, and failed pane.
- Active idle pane with a background working pane.
- Active working pane with a stashed Agent Pane waiting for permission.
- Multiple progress percentages are never averaged.
- Dominance ties choose the latest revision.
- Moving, closing, stashing, and restoring panes update membership correctly.

### Observation tests

- Background completion raises `Update`.
- Completion in a visible foreground split does not raise unread state.
- Selecting a tab does not acknowledge a stashed pane.
- Focusing the source pane acknowledges `Update` and `Error`.
- `ActionRequired` persists until the operation resumes or finishes.

### UI and accessibility tests

- Tab remains single-line at narrow widths.
- Indicator does not collide with title, icon, progress, bell, or close button.
- Automation names include state, pane identity, and count.
- Every state remains distinguishable without color.
- Reduced-motion settings disable activity animation.

## Success criteria

The V1 design succeeds when:

1. a pane can move through reliable idle, working, waiting, and completed
   states without parsing visible text;
2. a background or stashed pane can raise attention on its owning tab;
3. a multi-pane tab chooses the correct dominant state without changing its
   active-pane title;
4. the user can activate the indicator and arrive at the pane that caused it;
5. attention clears only when the relevant state is observed or resolved; and
6. existing title, progress, bell, connection, and Agent Pane behavior remains
   intact.
