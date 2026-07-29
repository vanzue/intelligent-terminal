use agent_client_protocol as acp;

pub mod providers;

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UsageSnapshot {
    pub context: Option<UsageContext>,
    pub cost: Option<UsageCost>,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UsageContext {
    pub used: u64,
    pub size: u64,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct UsageStaleness {
    pub context: bool,
    pub cost: bool,
}

impl UsageStaleness {
    pub fn mark_reported(&mut self, snapshot: &UsageSnapshot) {
        if snapshot.context.is_some() {
            self.context = false;
        }
        if snapshot.cost.is_some() {
            self.cost = false;
        }
    }

    pub fn mark_present_stale(&mut self, snapshot: &UsageSnapshot) {
        if snapshot.context.is_some() {
            self.context = true;
        }
        if snapshot.cost.is_some() {
            self.cost = true;
        }
    }
}

impl UsageSnapshot {
    pub fn merge(&mut self, incoming: Self) {
        if incoming.context.is_some() {
            self.context = incoming.context;
        }
        if incoming.cost.is_some() {
            self.cost = incoming.cost;
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UsageCost {
    pub amount_decimal_text: String,
    pub currency: String,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UsageProjection {
    pub items: Vec<UsageProjectionItem>,
}

#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize)]
pub struct UsageProjectionItem {
    pub metric_id: &'static str,
    pub value_decimal_text: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub limit_decimal_text: Option<String>,
    pub unit_id: String,
    pub scope: &'static str,
    pub source: &'static str,
    pub stale: bool,
}

impl From<&UsageSnapshot> for UsageProjection {
    fn from(snapshot: &UsageSnapshot) -> Self {
        Self::with_staleness(snapshot, UsageStaleness::default())
    }
}

impl UsageProjection {
    pub fn with_staleness(snapshot: &UsageSnapshot, staleness: UsageStaleness) -> Self {
        let mut items = Vec::with_capacity(2);
        if let Some(context) = &snapshot.context {
            items.push(UsageProjectionItem {
                metric_id: "acp.context.window",
                value_decimal_text: context.used.to_string(),
                limit_decimal_text: Some(context.size.to_string()),
                unit_id: "token".to_string(),
                scope: "session",
                source: "acp_standard",
                stale: staleness.context,
            });
        }
        if let Some(cost) = &snapshot.cost {
            items.push(UsageProjectionItem {
                metric_id: "acp.billing.cost",
                value_decimal_text: cost.amount_decimal_text.clone(),
                limit_decimal_text: None,
                unit_id: cost.currency.clone(),
                scope: "session",
                source: "acp_standard",
                stale: staleness.cost,
            });
        }
        Self { items }
    }
}

pub fn normalize_standard_usage(update: &acp::schema::v1::UsageUpdate) -> UsageSnapshot {
    let cost = update
        .cost
        .as_ref()
        .filter(|cost| cost.amount.is_finite() && !cost.amount.is_sign_negative())
        .map(|cost| UsageCost {
            amount_decimal_text: cost.amount.to_string(),
            currency: cost.currency.clone(),
        });

    UsageSnapshot {
        context: Some(UsageContext {
            used: update.used,
            size: update.size,
        }),
        cost,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use agent_client_protocol as acp;

    #[test]
    fn normalizes_standard_context_and_cumulative_cost() {
        let update = acp::schema::v1::UsageUpdate::new(1_024, 8_192)
            .cost(acp::schema::v1::Cost::new(0.004, "USD"));

        let snapshot = normalize_standard_usage(&update);

        assert_eq!(
            snapshot.context,
            Some(UsageContext {
                used: 1_024,
                size: 8_192
            })
        );
        assert_eq!(
            snapshot.cost,
            Some(UsageCost {
                amount_decimal_text: "0.004".to_string(),
                currency: "USD".to_string(),
            })
        );
    }

    #[test]
    fn normalizes_standard_context_without_cost() {
        let snapshot = normalize_standard_usage(&acp::schema::v1::UsageUpdate::new(20, 100));

        assert_eq!(
            snapshot.context,
            Some(UsageContext {
                used: 20,
                size: 100
            })
        );
        assert!(snapshot.cost.is_none());
    }

    #[test]
    fn projects_only_context_and_cost_metrics() {
        let snapshot = UsageSnapshot {
            context: Some(UsageContext {
                used: 1_024,
                size: 8_192,
            }),
            cost: Some(UsageCost {
                amount_decimal_text: "0.004".to_string(),
                currency: "USD".to_string(),
            }),
        };

        let projection = UsageProjection::from(&snapshot);

        assert_eq!(projection.items.len(), 2);
        assert_eq!(projection.items[0].metric_id, "acp.context.window");
        assert_eq!(projection.items[1].metric_id, "acp.billing.cost");
    }

    #[test]
    fn merges_independent_context_and_cost_snapshots() {
        let mut snapshot =
            normalize_standard_usage(&acp::schema::v1::UsageUpdate::new(1_024, 8_192));

        snapshot.merge(UsageSnapshot {
            context: None,
            cost: Some(UsageCost {
                amount_decimal_text: "0.004".to_string(),
                currency: "USD".to_string(),
            }),
        });

        assert_eq!(
            snapshot.context,
            Some(UsageContext {
                used: 1_024,
                size: 8_192
            })
        );
        assert_eq!(snapshot.cost.expect("cost").currency, "USD");
    }

    #[test]
    fn preserves_provider_reported_context_gauges() {
        for (used, size) in [(1, 0), (101, 100)] {
            let snapshot = normalize_standard_usage(&acp::schema::v1::UsageUpdate::new(used, size));

            assert_eq!(snapshot.context, Some(UsageContext { used, size }));
        }
    }

    #[test]
    fn omits_non_finite_or_negative_cost_without_discarding_context() {
        for amount in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY, -0.01] {
            let update = acp::schema::v1::UsageUpdate::new(1, 100)
                .cost(acp::schema::v1::Cost::new(amount, "USD"));
            let snapshot = normalize_standard_usage(&update);

            assert_eq!(snapshot.context, Some(UsageContext { used: 1, size: 100 }));
            assert!(snapshot.cost.is_none());
        }
    }

    #[test]
    fn preserves_provider_reported_currency_without_shape_policy() {
        for currency in ["US", "USDD", "usd", "US1", "U$D"] {
            let update = acp::schema::v1::UsageUpdate::new(1, 100)
                .cost(acp::schema::v1::Cost::new(1.0, currency));
            let snapshot = normalize_standard_usage(&update);

            assert_eq!(snapshot.context, Some(UsageContext { used: 1, size: 100 }));
            assert_eq!(
                snapshot.cost,
                Some(UsageCost {
                    amount_decimal_text: "1".to_string(),
                    currency: currency.to_string(),
                })
            );
        }
    }

    #[test]
    fn provider_registry_covers_every_known_agent_family() {
        let mut registered = providers::all()
            .iter()
            .map(|provider| provider.family_id())
            .collect::<Vec<_>>();
        registered.sort_unstable();

        let mut known = crate::agent_registry::KNOWN_AGENTS
            .iter()
            .map(|profile| profile.id)
            .collect::<Vec<_>>();
        known.sort_unstable();

        assert_eq!(registered, known);
    }

    #[test]
    fn provider_registry_declares_current_private_usage_policy() {
        use providers::PrivateUsagePolicy;

        assert_eq!(
            providers::lookup("copilot").unwrap().private_usage_policy(),
            PrivateUsagePolicy::Reserved
        );
        assert_eq!(
            providers::lookup("claude").unwrap().private_usage_policy(),
            PrivateUsagePolicy::StandardAcpOnly
        );
        assert_eq!(
            providers::lookup("codex").unwrap().private_usage_policy(),
            PrivateUsagePolicy::StandardAcpOnly
        );
        assert_eq!(
            providers::lookup("gemini").unwrap().private_usage_policy(),
            PrivateUsagePolicy::StandardAcpOnly
        );
        assert_eq!(
            providers::lookup("opencode")
                .unwrap()
                .private_usage_policy(),
            PrivateUsagePolicy::StandardAcpOnly
        );
    }

    #[test]
    fn provider_adapters_do_not_invent_unverified_private_usage() {
        let meta = serde_json::json!({ "unverified": { "amount": 12345 } });
        let notification = serde_json::json!({ "credits": 98765 });
        let inputs = [
            providers::ProviderUsageInput::SessionUpdateMeta(&meta),
            providers::ProviderUsageInput::PromptResponseMeta(&meta),
            providers::ProviderUsageInput::ExtensionNotification {
                method: "vendor/private-usage",
                params: &notification,
            },
            providers::ProviderUsageInput::ProviderApiResponse {
                schema_id: "vendor.usage.v1",
                body: &notification,
            },
        ];

        for provider in providers::all().iter().copied() {
            assert!(
                provider.trusted_reporter_ids().is_empty(),
                "{} must not trust a private reporter before wire verification",
                provider.family_id()
            );
            for reporter_id in [None, Some("lookalike-reporter")] {
                for input in &inputs {
                    assert_eq!(
                        provider
                            .extract_private_usage(providers::ProviderUsageRequest {
                                reporter_id,
                                input: *input,
                            })
                            .unwrap(),
                        providers::ProviderUsageContribution::default(),
                        "{} must stay no-op until its schema is verified",
                        provider.family_id()
                    );
                }
            }
        }
    }

    #[test]
    fn unknown_or_custom_agents_have_no_private_provider_adapter() {
        assert!(providers::lookup("unknown").is_none());
        assert!(providers::lookup("custom:npx").is_none());
    }
}
