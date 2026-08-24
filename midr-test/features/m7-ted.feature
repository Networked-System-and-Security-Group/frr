Feature: MIDR production TED delivery

  @M7-TED-001
  Scenario: Match mock and production TED data
    Given one normalized topology is supplied through mock and real paths
    When both paths publish immutable TED snapshots
    Then all six sorted TED arrays have identical content

  @M7-TED-002
  Scenario: Preserve the public consumer lifecycle
    Given a path consumer is registered through the public TED header
    When production TED generations are published
    Then callback, snapshot reference, generation and unregister semantics hold

  @M7-TED-003
  Scenario: Deliver propagated prefix reachability
    Given a prefix is originated by a remote MIDR group
    When its Node Prefix and representative Group Prefix cross real sessions
    Then the receiving production TED contains the prefix-to-group row
