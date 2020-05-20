#!/usr/bin/env bats

invoke_austin() {
  python_bin=$1
  ignore=$2
  
  if ! $python_bin -V; then skip "$python_bin not found."; fi

  for i in {1..3}
  do
    echo "> Run $i of 3"

    # -------------------------------------------------------------------------

    echo "  :: Standard profiling"
    run sudo src/austin -i 1000 -t 10000 $python_bin test/target34.py

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
    run sudo src/austin -i 1000 -t 10000 -m $python_bin test/target34.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q "keep_cpu_busy (test/target34.py);L"
    then
      continue
    fi
    echo "       Output: OK"

    # -------------------------------------------------------------------------

    echo "  :: Output file"
    run sudo src/austin -i 10000 -t 10000 -o /tmp/austin_out.txt $python_bin test/target34.py

    echo "       Exit code: $status"
    if [ $status != 0 ]; then continue; fi

    if ! echo "$output" | grep -q "Unwanted" \
    || cat /tmp/austin_out.txt | grep -q "keep_cpu_busy (test/target34.py);L"
    then
      echo "       Output: OK"
      return
    fi
  done

  if [ $ignore ]
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


teardown() {
  if [ -f /tmp/austin_out.txt ]; then rm /tmp/austin_out.txt; fi
}


# @test "Test Austin with the default Python 3" {
#   /usr/bin/python3 -m venv --copies --without-pip /tmp/py3
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
