//! `App`'s state-projection methods (echoing tab/agent state back to the
//! C++/XAML host), split out of the large `app.rs` file. Declared as a
//! regular (non-test) child module of `app` via `#[path]` so it can reach
//! `App`'s private fields and helper methods just like the rest of
//! `app.rs` does.

use super::*;

impl App {
    /// Push the current agent status (name / version / model / connection state)
    /// to the host so a XAML-rendered agent bar can update itself. The COM
    /// server special-cases `method == "agent_status"` and dispatches it
    /// straight to TerminalPage, parallel to the existing `autofix_state`
    /// path. Cheap to call on every state change — the publisher serializes
    /// `wtcli publish` invocations, and an extra one per state transition is
    /// negligible compared to chat traffic.
    pub(super) fn publish_agent_status(&mut self) {
        let state_str = match &self.state {
            ConnectionState::Connecting(_) => "connecting",
            ConnectionState::Connected => "connected",
            ConnectionState::Failed(_) => "failed",
            ConnectionState::Disconnected => "disconnected",
        };
        // Include selected_agent only once — when connected after user selection.
        // This avoids triggering _RebuildAgentStack mid-FRE.
        let selected = if self.state == ConnectionState::Connected {
            self.pending_agent_selection.take()
        } else {
            None
        };
        let display_model = self
            .current_model_display()
            .or_else(|| self.agent_model.clone());
        let mut params = serde_json::json!({
            "agent_id": self.current_agent_id,
            "name": self.agent_name,
            "version": self.agent_version,
            "model": display_model,
            "backend": self.current_agent_source.display_suffix(),
            "agent_source": self.current_agent_source.kind(),
            "state": state_str,
            "available_models": self.available_models,
            "current_model_id": self.current_model_id,
            "host_catalog_ready": self.host_catalog_ready,
        });
        if let Some(agent_id) = selected {
            params["selected_agent"] = serde_json::Value::String(agent_id);
        }
        // Tag with the helper's owned tab so C++ routes the title-bar
        // update to the right AgentPaneContent. Without this, OnAgentStatusChanged
        // fans the event out to every agent pane in every window — fine
        // for single-pane setups, broken once multiple helpers each
        // publish their own status (cross-tab title-bar clobber).
        if let Some(ref tab) = self.owner_tab_id {
            params["tab_id"] = serde_json::Value::String(tab.clone());
        }
        if let Some(ref pane) = self.pane_id {
            params["pane_id"] = serde_json::Value::String(pane.clone());
        }
        let evt = serde_json::json!({
            "type": "event",
            "method": "agent_status",
            "params": params,
        });
        send_wt_protocol_event(evt.to_string());
    }

    /// Single outbound projection of the active tab's agent-pane UI state.
    ///
    /// **Architecture contract**: per-tab agent-pane UI state lives in wta.
    /// C++ has one shared agent pane and one set of XAML flags per window,
    /// so anything that varies across WT tabs must be re-asserted on every
    /// tab switch or local mutation. Emits one unified `agent_state_changed`
    /// snapshot — adding a new piece of per-tab UI state in the future is
    /// a matter of putting another field in the payload, no new IDL route
    /// or new C++ handler.
    ///
    /// Payload shape (mirror of the inbound `set_agent_state` request):
    /// ```json
    /// {
    ///   "type": "event",
    ///   "method": "agent_state_changed",
    ///   "params": {
    ///     "view":      "chat" | "sessions",
    ///     "pane_open": true | false,
    ///     "pane_position": "left" | "right" | "up" | "bottom" | null
    ///   }
    /// }
    /// ```
    ///
    /// On the C++ side this lands in `TerminalPage::OnAgentStateChanged`,
    /// which is the single writer of `_agentSessionsViewActive` and
    /// `Tab.AgentPaneOpen` for the active tab.
    ///
    /// Also re-emits the autofix bar snapshot (orthogonal domain — bottom
    /// bar autofix indicator — kept on its own `autofix_state` route).
    ///
    /// Call sites:
    ///   - `switch_tab_session` end — covers WT `tab_changed`.
    ///   - `set_agent_state` handler end — echoes C++'s request back so C++
    ///     mirrors it (the round-trip the new architecture is built on).
    ///   - `load_session` after the per-tab mutation.
    ///   - Esc out of agent session view, `/sessions` slash command, Ctrl+C×2
    ///     multi-tab reset.
    ///   - Once at startup (after `--initial-view` has been applied) so
    ///     the bar and the agent-pane-open flag both pick up the spawn
    ///     intent.
    ///
    /// Idempotent — safe to call multiple times in a row.
    pub fn project_active_tab_state(&self) {
        let active = self.active_tab_key().to_string();
        self.project_tab_state(&active);
    }

    /// Project the given tab's state to C++ regardless of whether it is the
    /// active tab. Used by `set_agent_state` so a mutation targeting a
    /// non-active tab still echoes back — under per-tab routing C++ can
    /// apply state changes to any tab, not just the focused one, so
    /// the old "defer until next tab_changed" gate was wrong.
    pub fn project_tab_state(&self, target_tab: &str) {
        let Some(tab) = self.tab_sessions.get(target_tab) else {
            tracing::warn!(
                target: "project_tab_state",
                tab_id = %target_tab,
                "no tab_session for target — skipping echo"
            );
            return;
        };
        let evt = build_agent_state_changed_event(target_tab, self.pane_id.as_deref(), tab);
        send_wt_protocol_event(evt.to_string());

        // Autofix bar is window-level (single bottom bar reflecting the
        // active tab), so only re-emit when we're projecting the active
        // tab. A non-active mutation does not change the visible bar.
        if target_tab == self.active_tab_key() {
            send_bar_event(&tab.autofix.bar_snapshot, Some(target_tab));
        }
    }

    pub(super) fn project_changed_activity_states(&mut self) {
        self.last_projected_activity
            .retain(|tab_id, _| self.tab_sessions.contains_key(tab_id));
        let changes: Vec<_> = self
            .tab_sessions
            .iter()
            .filter_map(|(tab_id, tab)| {
                let event =
                    build_agent_state_changed_event(tab_id, self.pane_id.as_deref(), tab);
                let projection = serde_json::json!({
                    "pane_id": event["params"]["pane_id"],
                    "activity": event["params"]["activity"],
                });
                if self.last_projected_activity.get(tab_id) == Some(&projection) {
                    None
                } else {
                    Some((tab_id.clone(), event, projection))
                }
            })
            .collect();

        for (tab_id, event, projection) in changes {
            self.last_projected_activity.insert(tab_id, projection);
            send_wt_protocol_event(event.to_string());
        }
    }
}

pub(super) fn build_agent_state_changed_event(
    target_tab: &str,
    pane_id: Option<&str>,
    tab: &TabSession,
) -> serde_json::Value {
    let view = match tab.current_view {
        View::Agents => "sessions",
        View::Chat => "chat",
    };
    let usage = tab.usage.as_ref().map(|snapshot| {
        crate::usage::UsageProjection::with_staleness(snapshot, tab.usage_staleness)
    });
    let waiting = !tab.permission.is_empty() || !tab.user_input.is_empty();
    let phase = if waiting {
        "waiting"
    } else if tab.turn.is_in_flight() {
        "working"
    } else {
        "idle"
    };
    let operation_id = tab
        .turn
        .prompt()
        .map(|prompt| prompt.id)
        .unwrap_or(tab.activity_operation_id);
    let outcome = tab.activity_outcome.map(|outcome| outcome.as_str());
    let mut event = serde_json::json!({
        "type": "event",
        "method": "agent_state_changed",
        "params": {
            "tab_id": target_tab,
            "view": view,
            "pane_open": tab.pane_open,
            "pane_position": tab.agent_pane_position,
            "usage": usage,
            "activity": {
                "phase": phase,
                "outcome": outcome,
                "operation_id": operation_id,
                "summary": tab.activity_summary.as_deref(),
            },
        }
    });
    if let Some(pane_id) = pane_id {
        event["params"]["pane_id"] = serde_json::Value::String(pane_id.to_string());
    }
    event
}

#[cfg(test)]
mod tests {
    use super::*;

    fn prompt(id: u64) -> SubmittedPrompt {
        SubmittedPrompt {
            id,
            text: "test".into(),
            submitted_at_unix_s: 0.0,
            context: TurnContext::default(),
            autofix: None,
        }
    }

    #[test]
    fn activity_projection_tracks_turn_waiting_and_outcome() {
        let mut tab = TabSession {
            turn: TurnState::Submitted(prompt(42)),
            activity_operation_id: 42,
            activity_summary: Some("Inspect the repository".into()),
            ..Default::default()
        };

        let working = build_agent_state_changed_event("TAB-1", Some("PANE-1"), &tab);
        assert_eq!(working["params"]["pane_id"], "PANE-1");
        assert_eq!(working["params"]["activity"]["phase"], "working");
        assert_eq!(working["params"]["activity"]["operation_id"], 42);
        assert_eq!(
            working["params"]["activity"]["summary"],
            "Inspect the repository"
        );

        tab.user_input.push_back(UserInputState {
            request_id: "request".into(),
            request: crate::agent_tools::user_input::UserInputRequest {
                question: "Choose".into(),
                choices: vec![],
                allow_freeform: true,
            },
            selected: 0,
            input: String::new(),
            responder: None,
        });
        let waiting = build_agent_state_changed_event("TAB-1", Some("PANE-1"), &tab);
        assert_eq!(waiting["params"]["activity"]["phase"], "waiting");

        tab.user_input.clear();
        tab.turn = TurnState::Idle;
        tab.activity_outcome = Some(AgentActivityOutcome::Succeeded);
        let completed = build_agent_state_changed_event("TAB-1", Some("PANE-1"), &tab);
        assert_eq!(completed["params"]["activity"]["phase"], "idle");
        assert_eq!(completed["params"]["activity"]["outcome"], "succeeded");
    }
}
