Feature: MIDR immutable TED path-computation input

  @M1-TED-001
  Scenario: Same-group topology exposes directed router-level data
    Given the same-group fixture
    When the mock provider publishes a READY snapshot
    Then the snapshot matches the same-group expectation

  @M1-TED-002
  Scenario: Cross-group topology separates intra and egress links
    Given the cross-group fixture
    When the mock provider publishes a READY snapshot
    Then the snapshot contains the derived directed group edge

  @M1-TED-003
  Scenario: A directed Link does not create a reverse Link
    Given the directed-link fixture
    When the mock provider publishes a READY snapshot
    Then only the advertised direction is present

  @M1-TED-004
  Scenario: Multiple egress Links aggregate to the minimum directed cost
    Given the multiple-egress fixture
    When the mock provider publishes a READY snapshot
    Then one group edge uses the minimum member Link cost

  @M1-TED-005
  Scenario: One prefix can map to multiple target groups
    Given the multiple-target-groups fixture
    When the mock provider publishes a READY snapshot
    Then every prefix-group mapping is preserved

  @M1-TED-006
  Scenario: An unreachable target remains mapped without a group edge
    Given the unreachable fixture
    When the mock provider publishes a READY snapshot
    Then the target mapping exists and the group graph has no edge

  @M1-TED-007
  Scenario: Unknown YAML keys are rejected
    Given a fixture with an unknown top-level key
    When strict schema validation runs
    Then validation fails before the C provider is called

  @M1-TED-008
  Scenario: Duplicate Link identity is rejected
    Given a fixture with duplicate directed Link identity
    When strict schema validation runs
    Then validation fails before the C provider is called

  @M1-TED-009
  Scenario: Invalid canonical cost is rejected
    Given a fixture with zero Link cost
    When strict schema validation runs
    Then validation fails before the C provider is called

  @M1-TED-010
  Scenario: Unknown Link endpoint is rejected
    Given a fixture whose Link references an unknown node
    When strict schema validation runs
    Then validation fails before the C provider is called

  @M1-TED-011
  Scenario: Mixed Link endpoint address families are rejected
    Given a fixture whose Link endpoint families differ
    When strict schema validation runs
    Then validation fails before the C provider is called

  @M1-TED-012
  Scenario: Duplicate YAML mapping keys are rejected
    Given a fixture with a duplicated local_group_id key
    When strict schema validation runs
    Then validation fails before the C provider is called
