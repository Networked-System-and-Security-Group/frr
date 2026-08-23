Feature: MIDR prefix reachability and representative origination

  @M6-PFX-001
  Scenario: Classify local and external prefix contributors
    Given IPv4 and IPv6 unicast paths from configured sources
    When MIDR export policy is evaluated
    Then only valid permitted non-MIDR paths become contributors

  @M6-PFX-002
  Scenario: Originate node and group prefix objects
    Given a permitted local prefix and complete group membership
    When the local node becomes the group representative
    Then Node Prefix and Group Prefix objects reach a remote TED

  @M6-PFX-003
  Scenario: Withdraw the last prefix contributor
    Given a Group Prefix has one active contributor
    When the final contributor disappears
    Then Node Prefix and Group Prefix state is withdrawn hop by hop

  @M6-PFX-004
  Scenario: Delay representative takeover
    Given two group members contribute the same prefix
    When the current representative becomes unreachable
    Then the next representative originates after the takeover delay

  @M6-PFX-005
  Scenario: Recover prefix input from an out-of-sync state
    Given Prefix input has published Node and Group Prefix objects
    When Prefix input becomes unreliable and is rescanned
    Then the objects are withdrawn and atomically restored
