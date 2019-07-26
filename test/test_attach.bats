attach_austin_2_3() {
  if ! python$1 -V; then skip "Python $1 not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Standard profiling"
    python$1 test/sleepy.py &
    sleep 1
    run src/austin -i 100000 -t 10000 -p $!
    echo "       Exit code: $status"
    [ $status = 0 ]
    if ! echo "$output" | grep -q ";? (test/sleepy.py);L13 "
    then
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    echo "  :: Memory profiling"
    python$1 test/sleepy.py &
    sleep 1
    run src/austin -mi 100 -t 10000 -p $!
    echo "       Exit code: $status"
    [ $status = 0 ]
    if echo "$output" | grep -q "cpu_bound"
    then
      echo "       Output: OK"
      return
    fi
  done

  false
}

attach_austin() {
  if ! python$1 -V; then skip "Python $1 not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Standard profiling"
    python$1 test/sleepy.py &
    sleep 1
    run src/austin -i 10000 -t 10000 -p $!
    echo "       Exit code: $status"
    [ $status = 0 ]
    if ! echo "$output" | grep -q ";<module> (test/sleepy.py);L13 "
    then
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    python$1 test/sleepy.py &
    sleep 1
    run src/austin -mi 100 -t 10000 -p $!
    echo "       Exit code: $status"
    [ $status = 0 ]
    if echo "$output" | grep -q "cpu_bound"
    then
      echo "       Output: OK"
      return
    fi
  done

  false
}


# -----------------------------------------------------------------------------


@test "Test Austin with Python 2.3" {
  skip "Disabled"
	attach_austin_2_3 "2.3"
}

@test "Test Austin with Python 2.4" {
	attach_austin_2_3 "2.4"
}

@test "Test Austin with Python 2.5" {
	attach_austin "2.5"
}

@test "Test Austin with Python 2.6" {
	attach_austin "2.6"
}

@test "Test Austin with Python 2.7" {
	attach_austin "2.7"
}

@test "Test Austin with Python 3.3" {
	attach_austin "3.3"
}

@test "Test Austin with Python 3.4" {
	attach_austin "3.4"
}

@test "Test Austin with Python 3.5" {
	attach_austin "3.5"
}

@test "Test Austin with Python 3.6" {
  attach_austin "3.6"
}

@test "Test Austin with Python 3.7" {
  attach_austin "3.7"
}
