#!/usr/bin/env bats

invoke_austin() {
  if ! python$1 -V; then skip "Python $1 not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Valgrind test"
    run valgrind \
      --error-exitcode=42 \
      --leak-check=full \
      --show-leak-kinds=all \
      --errors-for-leak-kinds=all \
      --track-fds=yes \
      src/austin -i 100000 -t 10000 python$1 test/target34.py
    echo "       Exit code: $status"
    echo "       Valgrind report: <"
    echo "$output"
  	if [ $status = 0 ]
    then
      return
    fi
  done

  if [ $2 ]
  then
    skip "Test failed but marked as 'Ignore'"
  else
    echo
    echo "Collected Output"
    echo "================"
    echo
    echo "$output"
    echo
    false
  fi
}


# -----------------------------------------------------------------------------


@test "Test Austin with Python 2.3" {
	invoke_austin "2.3" ignore
}

@test "Test Austin with Python 2.4" {
	invoke_austin "2.4" ignore
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

@test "Test Austin with Python 3.8" {
  invoke_austin "3.8"
}
