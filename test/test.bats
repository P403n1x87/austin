#!/usr/bin/env bats

test_case() {
  run bats test/test_$1.bats
  echo $output
  [ $status = 0 ]
}

@test "Test Austin: fork" {
  test_case fork
}

@test "Test Austin: attach" {
  test_case attach
}
