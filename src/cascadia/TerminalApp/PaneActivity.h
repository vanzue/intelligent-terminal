// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TerminalApp::PaneActivity
{
    enum class Availability
    {
        Connecting,
        Ready,
        Disconnected,
    };

    enum class Phase
    {
        Unknown,
        Idle,
        Working,
        Waiting,
    };

    enum class Outcome
    {
        None,
        Succeeded,
        Failed,
        Cancelled,
        Interrupted,
    };

    enum class Attention
    {
        None,
        Update,
        Error,
        ActionRequired,
    };

    enum class OperationKind
    {
        None,
        Shell,
        Application,
        Agent,
    };

    enum class ProgressState
    {
        None,
        Indeterminate,
        Percent,
        Paused,
    };

    enum class SignalKind
    {
        ConnectionReady,
        ConnectionClosed,
        PromptStarted,
        InputStarted,
        OperationStarted,
        OperationWaiting,
        OperationResumed,
        OperationFinished,
        ProgressChanged,
        Bell,
        Observed,
    };

    struct Signal
    {
        SignalKind kind{ SignalKind::Observed };
        OperationKind operationKind{ OperationKind::None };
        Outcome outcome{ Outcome::None };
        ProgressState progressState{ ProgressState::None };
        uint32_t progressValue{ 0 };
        uint64_t operationId{ 0 };
        bool observed{ false };
        std::wstring summary;
        std::wstring command;
        std::optional<uint32_t> exitCode;
    };

    struct State
    {
        Availability availability{ Availability::Connecting };
        Phase phase{ Phase::Unknown };
        Outcome lastOutcome{ Outcome::None };
        Attention attention{ Attention::None };
        OperationKind operationKind{ OperationKind::None };
        OperationKind lastOperationKind{ OperationKind::None };
        ProgressState progressState{ ProgressState::None };
        uint32_t progressValue{ 0 };
        uint64_t operationId{ 0 };
        uint64_t revision{ 0 };
        uint64_t attentionRevision{ 0 };
        bool hasOperation{ false };
        std::wstring summary;
        std::wstring lastCommand;
        std::optional<uint32_t> lastExitCode;
    };

    struct PaneState
    {
        uint32_t paneId{ 0 };
        State state;
    };

    struct Aggregate
    {
        Phase phase{ Phase::Unknown };
        Attention attention{ Attention::None };
        uint32_t dominantPaneId{ 0 };
        uint32_t activePaneCount{ 0 };
        uint32_t attentionPaneCount{ 0 };
        std::wstring summary;
    };

    constexpr int AttentionPriority(const Attention attention) noexcept
    {
        switch (attention)
        {
        case Attention::ActionRequired:
            return 3;
        case Attention::Error:
            return 2;
        case Attention::Update:
            return 1;
        default:
            return 0;
        }
    }

    constexpr int PhasePriority(const Phase phase) noexcept
    {
        switch (phase)
        {
        case Phase::Waiting:
            return 2;
        case Phase::Working:
            return 1;
        default:
            return 0;
        }
    }

    constexpr bool IsPaneObserved(const bool tabFocused,
                                  const uint32_t paneId,
                                  const std::optional<uint32_t> activePaneId,
                                  const bool hidden) noexcept
    {
        return tabFocused && !hidden && activePaneId.has_value() && *activePaneId == paneId;
    }

    inline void ApplySignal(State& state, const Signal& signal)
    {
        ++state.revision;

        const auto setAttention = [&](const Attention attention) {
            state.attention = attention;
            state.attentionRevision = state.revision;
        };

        switch (signal.kind)
        {
        case SignalKind::ConnectionReady:
            if (state.availability == Availability::Disconnected)
            {
                state.attention = Attention::None;
                state.lastOutcome = Outcome::None;
                state.summary.clear();
            }
            state.availability = Availability::Ready;
            if (state.phase == Phase::Unknown)
            {
                state.phase = Phase::Idle;
            }
            break;
        case SignalKind::ConnectionClosed:
            state.availability = Availability::Disconnected;
            state.phase = Phase::Idle;
            state.hasOperation = false;
            state.operationKind = OperationKind::None;
            state.progressState = ProgressState::None;
            state.lastOutcome = Outcome::Interrupted;
            state.summary = signal.summary;
            setAttention(Attention::Error);
            break;
        case SignalKind::PromptStarted:
            if (state.hasOperation && state.operationKind == OperationKind::Shell)
            {
                state.lastOutcome = Outcome::Interrupted;
                state.hasOperation = false;
            }
            state.phase = Phase::Idle;
            state.progressState = ProgressState::None;
            break;
        case SignalKind::InputStarted:
            if (!state.hasOperation)
            {
                state.phase = Phase::Idle;
            }
            break;
        case SignalKind::OperationStarted:
            state.availability = Availability::Ready;
            state.phase = Phase::Working;
            state.lastOutcome = Outcome::None;
            state.operationKind = signal.operationKind;
            state.lastOperationKind = signal.operationKind;
            state.operationId = signal.operationId == 0 ? state.operationId + 1 : signal.operationId;
            state.hasOperation = true;
            state.progressState = ProgressState::None;
            state.summary = signal.summary;
            state.lastCommand.clear();
            state.lastExitCode.reset();
            state.attention = Attention::None;
            break;
        case SignalKind::OperationWaiting:
            state.phase = Phase::Waiting;
            state.operationKind = signal.operationKind;
            state.lastOperationKind = signal.operationKind;
            state.operationId = signal.operationId == 0 ? state.operationId : signal.operationId;
            state.hasOperation = true;
            state.summary = signal.summary;
            setAttention(Attention::ActionRequired);
            break;
        case SignalKind::OperationResumed:
            if (state.hasOperation)
            {
                state.phase = Phase::Working;
                state.attention = Attention::None;
            }
            break;
        case SignalKind::OperationFinished:
            if (state.hasOperation || signal.outcome == Outcome::Failed)
            {
                state.phase = Phase::Idle;
                state.hasOperation = false;
                state.operationKind = OperationKind::None;
                state.progressState = ProgressState::None;
                state.lastOutcome = signal.outcome;
                state.summary = signal.summary;
                state.lastCommand = signal.command;
                state.lastExitCode = signal.exitCode;
                if (signal.outcome == Outcome::Failed)
                {
                    setAttention(Attention::Error);
                }
                else if (signal.outcome == Outcome::Succeeded && !signal.observed)
                {
                    setAttention(Attention::Update);
                }
                else
                {
                    state.attention = Attention::None;
                }
            }
            break;
        case SignalKind::ProgressChanged:
            state.progressState = signal.progressState;
            state.progressValue = signal.progressValue;
            if (signal.progressState != ProgressState::None)
            {
                if (!state.hasOperation)
                {
                    state.operationId++;
                    state.operationKind = OperationKind::Application;
                    state.lastOperationKind = OperationKind::Application;
                    state.hasOperation = true;
                }
                state.phase = signal.progressState == ProgressState::Paused ? Phase::Waiting : Phase::Working;
            }
            else if (state.hasOperation && state.operationKind == OperationKind::Application)
            {
                state.phase = Phase::Idle;
                state.hasOperation = false;
                state.operationKind = OperationKind::None;
                state.lastOutcome = Outcome::Succeeded;
                if (!signal.observed)
                {
                    setAttention(Attention::Update);
                }
            }
            break;
        case SignalKind::Bell:
            if (!signal.observed && state.attention == Attention::None)
            {
                setAttention(Attention::Update);
            }
            break;
        case SignalKind::Observed:
            if (state.attention == Attention::Update || state.attention == Attention::Error)
            {
                state.attention = Attention::None;
            }
            break;
        }
    }

    inline Aggregate AggregateStates(const std::vector<PaneState>& panes, const uint32_t activePaneId)
    {
        Aggregate result;
        const PaneState* dominant = nullptr;

        for (const auto& pane : panes)
        {
            const auto active = pane.state.phase == Phase::Working ||
                                pane.state.phase == Phase::Waiting ||
                                pane.state.attention != Attention::None;
            if (active)
            {
                ++result.activePaneCount;
            }
            if (pane.state.attention != Attention::None)
            {
                ++result.attentionPaneCount;
            }

            if (!dominant)
            {
                dominant = &pane;
                continue;
            }

            const auto paneAttention = AttentionPriority(pane.state.attention);
            const auto dominantAttention = AttentionPriority(dominant->state.attention);
            const auto panePhase = PhasePriority(pane.state.phase);
            const auto dominantPhase = PhasePriority(dominant->state.phase);
            const auto paneWins = paneAttention > dominantAttention ||
                                  (paneAttention == dominantAttention && panePhase > dominantPhase) ||
                                  (paneAttention == dominantAttention && panePhase == dominantPhase &&
                                   pane.state.revision > dominant->state.revision) ||
                                  (paneAttention == dominantAttention && panePhase == dominantPhase &&
                                   pane.state.revision == dominant->state.revision &&
                                   pane.paneId == activePaneId);
            if (paneWins)
            {
                dominant = &pane;
            }
        }

        if (dominant)
        {
            result.phase = dominant->state.phase;
            result.attention = dominant->state.attention;
            result.dominantPaneId = dominant->paneId;
            result.summary = dominant->state.summary;
        }

        return result;
    }
}
