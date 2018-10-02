#!/usr/bin/env bats

invoke_austin() {
  run austin -i 10000 python$1 test/target34.py
	[ $status = 0 ]
  echo $output | grep "keep_cpu_busy"
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
