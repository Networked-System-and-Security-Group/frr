Feature: MIDR multi-node link-state propagation

  @M5-PROP-001
  Scenario: Propagate an object across a linear topology
    Given three MIDR peers in one group form a line
    When the first peer originates a Link Object
    Then the third peer installs the object in its LSDB

  @M5-PROP-002
  Scenario: Withdraw an object hop by hop
    Given a Link Object has crossed two MIDR sessions
    When its owner withdraws the object
    Then every downstream RIB and LSDB removes it

  @M5-PROP-003
  Scenario: Enforce intra-group and global scope
    Given two peers are in one group and a third peer is in another
    When same-group and cross-group Link Objects are originated
    Then only the cross-group object reaches the other group

  @M5-PROP-004
  Scenario: Preserve an alternate path without looping
    Given three MIDR peers form a triangle
    When a direct MIDR adjacency is removed
    Then the indirect copy remains selected without path growth

  @M5-PROP-005
  Scenario: Recover from an initial EoR timeout
    Given one initial peer has not completed its local view
    When the EoR timeout expires and the peer later becomes ready
    Then the TED reason is degraded and later cleared

  @M5-PROP-006
  Scenario: Re-advertise the selected table on Route Refresh
    Given a downstream peer has a complete MIDR view
    When it requests an inbound Route Refresh
    Then the same identities remain selected without conflicts
