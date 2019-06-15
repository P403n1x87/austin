#!/usr/bin/env bats

invoke_austin() {
  if ! python$1 -V; then return; fi

  run src/austin -i 10000 -t 10000 python$1 test/target34.py
	[ $status = 0 ]
  echo $output | grep "keep_cpu_busy (test/target34.py);L7 "
  echo $output | grep "keep_cpu_busy (test/target34.py);L8 "

  # Memory profiling
  run src/austin -i 100000 -t 10000 -m python$1 test/target34.py
	[ $status = 0 ]
  echo $output | grep "keep_cpu_busy (test/target34.py);L"
}

@test "Test Austin with Python 2.3" {
	invoke_austin "2.3"
}

@test "Test Austin with Python 2.4" {
	invoke_austin "2.4"
}

@test "Test Austin with Python 2.5" {
	invoke_austin "2.5"
}

@test "Test Austin with Python 2.6" {
	invoke_austin "2.6"
}

@test "Test Austin with Python 2.7" {
	invoke_austin "2.7"
}

@test "Test Austin with Python 3.3" {
	invoke_austin "3.3"
}

@test "Test Austin with Python 3.4" {
	invoke_austin "3.4"
}

@test "Test Austin with Python 3.5" {
	invoke_austin "3.5"
}

@test "Test Austin with Python 3.6" {
  invoke_austin "3.6"
}

@test "Test Austin with Python 3.7" {
  invoke_austin "3.7"
}

# @test "Test Austin with Python 3.8" {
#   invoke_austin "3.8"
# }
