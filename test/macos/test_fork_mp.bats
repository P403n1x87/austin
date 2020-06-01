invoke_austin() {
  python_bin=$1
  ignore=$2

  if ! $python_bin -V; then skip "$python_bin not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Profiling of multi-process program"
    run sudo src/austin -i 100000 -C $python_bin test/target_mp.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    echo "       - Check expected number of processes."
    expected=3
    n_procs=$( echo "$output" | sed -E 's/P([0-9]+);.+/\1/' | sort | uniq | wc -l )
    echo "         Expected at least $expected and got $n_procs"
    if [ $n_procs -lt $expected ]; then continue; fi

    echo "       - Check output contains frames."
    if echo "$output" | grep -q "fact"
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


# @test "Test Austin with the default Python 3" {
#   /usr/bin/python3 -m venv /tmp/py3
#   source /tmp/py3/bin/activate
# 	invoke_austin "python3"
#   test -d /tmp/py3 && rm -rf /tmp/py3
# }

@test "Test Austin with default Python 3 from Homebrew" {
	invoke_austin "/usr/local/bin/python3"
}

@test "Test Austin with Python 3.8 from Homebrew (if available)" {
  invoke_austin "/usr/local/opt/python@3.8/bin/python3" ignore
}

@test "Test Austin with Python 3 from Anaconda (if available)" {
  invoke_austin "/usr/local/anaconda3/bin/python" ignore
}