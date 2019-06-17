#!/usr/bin/env bats

test_case() {
  run bats test/test_$1.bats
  echo "$output"
  [ $status = 0 ]
}

@test "Test Austin: fork" {
  test_case fork
}

@test "Test Austin: attach" {
  if [[ $EUID -ne 0 ]]; then
   skip "requires root"
  fi
  test_case attach
}

@test "Test Austin: valgrind" {
  test_case valgrind
}
