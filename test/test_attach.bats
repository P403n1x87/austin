attach_austin() {
  python$1 test/sleepy.py &
  sleep 5
  run ./austin -i 10000 -p $!
  [ $status = 0 ]
  echo $output | grep "cpu_bound"
  echo $output | grep ";<module> (test/sleepy.py);L13 "
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
