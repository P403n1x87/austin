#!/usr/bin/env bats

invoke_austin() {
  if ! python$1 -V; then skip "Python $1 not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Standard profiling"
    run src/austin -i 1000 -t 10000 python$1 test/target34.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q "keep_cpu_busy (test/target34.py);L" \
    || echo "$output" | grep -q "Unwanted"
    then
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    echo "  :: Memory profiling"
    run src/austin -i 1000 -t 10000 -m python$1 test/target34.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q "keep_cpu_busy (test/target34.py);L"
    then
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    echo "  :: Output file"
    run src/austin -i 10000 -t 10000 -o /tmp/austin_out.txt python$1 test/target34.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q "Unwanted" \
    || cat /tmp/austin_out.txt | grep -q "keep_cpu_busy (test/target34.py);L"
    then
      echo "       Output: OK"
      return
    fi
  done

  if [ $2 ]
  then
    skip "Test failed but marked as 'Ignore'"
  else
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

# @test "Test Austin with Python 3.8" {
#   invoke_austin "3.8"
# }
