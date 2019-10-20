#!/usr/bin/env bats

invoke_austin() {
  if ! python$1 -V; then skip "Python $1 not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Profiling of multi-process program"
    run src/austin -i 10000 -C python$1 test/target_mp.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    echo "       - Check expected number of processes."
    expected=3
    n_procs=$( echo "$output" | sed -r 's/Process ([0-9]+);.+/\1/' | sort | uniq | wc -l )
    echo "         Expected $expected and got $n_procs"
    if [ $n_procs != $expected ]
    then continue; fi

    echo "       - Check output contains frames."
    if echo "$output" | grep -q "do (test/target_mp.py);L[[:digit:]]*;fact (test/target_mp.py);L"
    then
      echo "       Output: OK"
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
  skip "Multiprocessing library introduced in Python 2.6"
	invoke_austin "2.3"
}

@test "Test Austin with Python 2.4" {
  skip "Multiprocessing library introduced in Python 2.6"
	invoke_austin "2.4"
}

@test "Test Austin with Python 2.5" {
  skip "Multiprocessing library introduced in Python 2.6"
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
