Feature: MIDR Local Fact snapshot and resynchronization

  @M2-INPUT-001
  Scenario: Snapshot and concurrent increment form one complete baseline
    Given an active Local Fact table and a pending pre-barrier event
    When a valid Provider Snapshot arrives with a post-barrier increment
    Then the snapshot and increment are committed atomically in FIFO order

  @M2-INPUT-002
  Scenario: A temporarily unavailable Provider does not create a state gap
    Given a complete committed Local Fact table
    When the Provider first returns EAGAIN and later returns a valid Snapshot
    Then the old table remains visible until the complete new table is committed

  @M2-INPUT-003
  Scenario: An invalid Snapshot rolls back as a whole
    Given a complete committed Local Fact table
    When the Provider returns malformed or inconsistent Snapshot data
    Then no partial Snapshot state replaces the committed table

  @M2-INPUT-004
  Scenario: Resync queue overflow cannot publish partial staging state
    Given a Resync Barrier with a bounded post-barrier queue
    When concurrent increments overflow that queue during Snapshot acquisition
    Then the staging table is discarded and the input enters OUT_OF_SYNC

  @M2-INPUT-005
  Scenario: Router-ID change establishes a new identity baseline
    Given Local Facts and tombstones owned by the old Router-ID
    When the Router-ID changes and the new Provider Snapshot arrives
    Then old identity state is removed and lower new-identity versions are accepted
