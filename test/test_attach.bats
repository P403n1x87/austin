attach_austin_2_3() {
  python$1 test/sleepy.py &
  sleep 5
  run src/austin -i 1000 -t 1000 -p $!
  [ $status = 0 ]
  echo $output | grep ";? (test/sleepy.py);L13 "
}

attach_austin() {
  python$1 test/sleepy.py &
  sleep 5
  run src/austin -i 1000 -t 1000 -p $!
  [ $status = 0 ]
  echo $output | grep "cpu_bound"
  echo $output | grep ";<module> (test/sleepy.py);L13 "
}

@test "Test Austin with Python 2.3" {
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
