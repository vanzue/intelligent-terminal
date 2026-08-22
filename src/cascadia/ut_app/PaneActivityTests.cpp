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
        TEST_METHOD(DeliveryPolicyFiltersActiveTabAndRoutineActivity);
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
        working.receivedSequence = 10;

        State failed;
        Signal failedSignal{ SignalKind::OperationFinished };
        failedSignal.outcome = Outcome::Failed;
        ApplySignal(failed, failedSignal);
        failed.receivedSequence = 11;

        State waiting;
        ApplySignal(waiting, { SignalKind::OperationWaiting, OperationKind::Agent });
        waiting.receivedSequence = 12;

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

        State olderWorking;
        ApplySignal(olderWorking, { SignalKind::OperationStarted, OperationKind::Shell });
        olderWorking.revision = 100;
        olderWorking.receivedSequence = 20;

        State newerWorking;
        ApplySignal(newerWorking, { SignalKind::OperationStarted, OperationKind::Agent });
        newerWorking.revision = 1;
        newerWorking.receivedSequence = 21;

        const auto newest = AggregateStates({
                                                { 4, olderWorking },
                                                { 5, newerWorking },
                                            },
                                            4);
        VERIFY_ARE_EQUAL(5u, newest.dominantPaneId);
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

    void PaneActivityTests::DeliveryPolicyFiltersActiveTabAndRoutineActivity()
    {
        using namespace TerminalApp::PaneActivity;

        State working;
        working.phase = Phase::Working;
        VERIFY_IS_FALSE(ShouldDeliver(working, DeliveryMode::AttentionOnly, false));
        VERIFY_IS_TRUE(ShouldDeliver(working, DeliveryMode::AllActivity, false));

        State completed;
        completed.attention = Attention::Update;
        VERIFY_IS_FALSE(ShouldDeliver(completed, DeliveryMode::AttentionOnly, false));
        VERIFY_IS_TRUE(ShouldDeliver(completed, DeliveryMode::AllActivity, false));

        State failed;
        failed.attention = Attention::Error;
        VERIFY_IS_TRUE(ShouldDeliver(failed, DeliveryMode::AttentionOnly, false));
        VERIFY_IS_FALSE(ShouldDeliver(failed, DeliveryMode::AttentionOnly, true));

        State waiting;
        waiting.phase = Phase::Waiting;
        waiting.attention = Attention::ActionRequired;
        VERIFY_IS_TRUE(ShouldDeliver(waiting, DeliveryMode::AttentionOnly, false));
        VERIFY_IS_FALSE(ShouldDeliver(waiting, DeliveryMode::AllActivity, true));
    }
}
