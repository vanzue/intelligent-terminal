// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../TerminalApp/PaneActivity.h"

using namespace WEX::TestExecution;

namespace TerminalAppUnitTests
{
    class PaneActivityTests
    {
        TEST_CLASS(PaneActivityTests);

        TEST_METHOD(ReducerPreservesAttentionSemantics);
        TEST_METHOD(AggregationSelectsHighestPriorityPane);
        TEST_METHOD(ObservationRequiresFocusedSourcePane);
    };

    void PaneActivityTests::ReducerPreservesAttentionSemantics()
    {
        using namespace TerminalApp::PaneActivity;

        State state;
        ApplySignal(state, { SignalKind::ConnectionReady });
        VERIFY_ARE_EQUAL(static_cast<int>(Phase::Idle), static_cast<int>(state.phase));

        ApplySignal(state, { SignalKind::OperationStarted, OperationKind::Shell });
        VERIFY_ARE_EQUAL(static_cast<int>(Phase::Working), static_cast<int>(state.phase));
        VERIFY_IS_TRUE(state.hasOperation);

        Signal finished{ SignalKind::OperationFinished };
        finished.outcome = Outcome::Succeeded;
        ApplySignal(state, finished);
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::Update), static_cast<int>(state.attention));
        VERIFY_ARE_EQUAL(static_cast<int>(OperationKind::Shell), static_cast<int>(state.lastOperationKind));

        ApplySignal(state, { SignalKind::Observed });
        ApplySignal(state, { SignalKind::OperationWaiting, OperationKind::Agent });
        ApplySignal(state, { SignalKind::Observed });
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::ActionRequired), static_cast<int>(state.attention));

        ApplySignal(state, { SignalKind::OperationResumed, OperationKind::Agent });
        VERIFY_ARE_EQUAL(static_cast<int>(Phase::Working), static_cast<int>(state.phase));
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::None), static_cast<int>(state.attention));

        finished.outcome = Outcome::Failed;
        finished.command = L"cargo test";
        finished.exitCode = 101;
        ApplySignal(state, finished);
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::Error), static_cast<int>(state.attention));
        VERIFY_ARE_EQUAL(L"cargo test", state.lastCommand);
        VERIFY_IS_TRUE(state.lastExitCode.has_value());
        VERIFY_ARE_EQUAL(101u, *state.lastExitCode);

        ApplySignal(state, { SignalKind::Observed });
        ApplySignal(state, { SignalKind::OperationStarted, OperationKind::Agent });
        VERIFY_IS_TRUE(state.lastCommand.empty());
        VERIFY_IS_FALSE(state.lastExitCode.has_value());
        finished.outcome = Outcome::Cancelled;
        ApplySignal(state, finished);
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::None), static_cast<int>(state.attention));
    }

    void PaneActivityTests::AggregationSelectsHighestPriorityPane()
    {
        using namespace TerminalApp::PaneActivity;

        State working;
        ApplySignal(working, { SignalKind::OperationStarted, OperationKind::Shell });

        State failed;
        Signal failedSignal{ SignalKind::OperationFinished };
        failedSignal.outcome = Outcome::Failed;
        ApplySignal(failed, failedSignal);

        State waiting;
        ApplySignal(waiting, { SignalKind::OperationWaiting, OperationKind::Agent });

        const auto aggregate = AggregateStates({
                                                   { 1, working },
                                                   { 2, failed },
                                                   { 3, waiting },
                                               },
                                               1);

        VERIFY_ARE_EQUAL(3u, aggregate.dominantPaneId);
        VERIFY_ARE_EQUAL(3u, aggregate.activePaneCount);
        VERIFY_ARE_EQUAL(2u, aggregate.attentionPaneCount);
        VERIFY_ARE_EQUAL(static_cast<int>(Attention::ActionRequired), static_cast<int>(aggregate.attention));
    }

    void PaneActivityTests::ObservationRequiresFocusedSourcePane()
    {
        using namespace TerminalApp::PaneActivity;

        VERIFY_IS_TRUE(IsPaneObserved(true, 2, 2, false));
        VERIFY_IS_FALSE(IsPaneObserved(true, 2, 1, false));
        VERIFY_IS_FALSE(IsPaneObserved(false, 2, 2, false));
        VERIFY_IS_FALSE(IsPaneObserved(true, 2, 2, true));
        VERIFY_IS_FALSE(IsPaneObserved(true, 2, std::nullopt, false));
    }
}
