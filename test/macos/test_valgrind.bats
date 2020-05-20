invoke_austin() {
  python_bin=$1
  ignore=$2

  if ! $python_bin -V; then skip "$python not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Valgrind test"
    run sudo valgrind \
      --error-exitcode=42 \
      --leak-check=full \
      --show-leak-kinds=all \
      --errors-for-leak-kinds=all \
      --track-fds=yes \
      src/austin -i 100000 -t 10000 $python_bin test/target34.py
    echo "       Exit code: $status"
    echo "       Valgrind report: <"
    echo "$output"
  	if [ $status = 0 ]
    then
      return
    fi
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
	invoke_austin "python"
  test -d /tmp/py3 && rm -rf /tmp/py3
}

@test "Test Austin with default Python 3 from Homebrew" {
	invoke_austin "/usr/local/bin/python3"
}

@test "Test Austin with Python 3.8 from Homebrew (if available)" {
  invoke_austin "/usr/local/opt/python@3.8/bin/python3" ignore
}

@test "Test Austin with Python 3 from Anaconda (if available)" {
  invoke_austin "/usr/local/anaconda3/bin/python" ignore
}
