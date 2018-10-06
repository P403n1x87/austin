#!/usr/bin/env bats

invoke_austin() {
  run valgrind \
    --error-exitcode=42 \
    --leak-check=full \
    --show-leak-kinds=all \
    --errors-for-leak-kinds=all \
    --track-fds=yes \
    src/austin -i 1000 python$1 test/target34.py
  echo "Exit code:" $status
	[ $status = 0 ]
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
