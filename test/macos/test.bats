test_case() {
  run bats test/macos/test_$1.bats
  echo "$output"
  [ $status = 0 ]
}

@test "Test Austin: fork" {
  test_case fork
}

@test "Test Austin: fork multi-process" {
  test_case fork_mp
}

@test "Test Austin: attach" {
  test_case attach
}

@test "Test Austin: valgrind" {
  skip "We skip valgrind on Mac OS for now"
  
  if ! which valgrind; then skip "Valgrind not found"; fi

  test_case valgrind
}
