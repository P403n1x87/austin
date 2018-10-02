#!/usr/bin/env bats

@test "Test Austin: fork" {
  run bats test/test_fork.bats
  [ $status = 0 ]
}

@test "Test Austin: attach" {
  run bats test/test_attach.bats
  [ $status = 0 ]
}
