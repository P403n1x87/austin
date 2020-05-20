attach_austin() {
  python_bin=$1
  ignore=$2

  if ! $python_bin -V; then skip "$python_bin not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Time profiling"
    $python_bin test/sleepy.py &
    sleep 1
    run sudo src/austin -i 10000 -t 10000 -p $!

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q ";<module> (test/sleepy.py);L[[:digit:]]* "
    then
      echo "       Output: NOK"
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    echo "  :: Memory profiling"
    $python_bin test/sleepy.py &
    sleep 1
    run sudo src/austin -mi 100 -t 10000 -p $!

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if echo "$output" | grep -q "Thread "
    then
      echo "       Output: OK"
      return
    fi
    echo "       Output: NOK"
  done

  if [ $ignore ]
  then
    echo "Test marked as 'Ignore' failed"
  fi
  echo
  echo "Collected Output"
  echo "================"
  echo
  echo "$output"
  echo
  
  false
}


# -----------------------------------------------------------------------------


@test "Test Austin with the default Python 3" {
  python -m venv /tmp/py3
  source /tmp/py3/bin/activate
	attach_austin "python"
  test -d /tmp/py3 && rm -rf /tmp/py3
}

@test "Test Austin with default Python 3 from Homebrew" {
	attach_austin "/usr/local/bin/python3"
}

@test "Test Austin with Python 3.8 from Homebrew (if available)" {
  attach_austin "/usr/local/opt/python@3.8/bin/python3" ignore
}

@test "Test Austin with Python 3 from Anaconda (if available)" {
  attach_austin "/usr/local/anaconda3/bin/python" ignore
}
