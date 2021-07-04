# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2019 Gabriele N. Tornetta <phoenix1987@gmail.com>.
# All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

# -----------------------------------------------------------------------------
# -- Austin
# -----------------------------------------------------------------------------

AUSTIN=`test -f src/austin.exe && echo "src/austin.exe" || echo "austin.exe"`


# -----------------------------------------------------------------------------
# -- Python
# -----------------------------------------------------------------------------

function check_python {
  if ! python -V; then skip "Python not found."; fi

  PYTHON="python"
}

# -----------------------------------------------------------------------------
# -- Logging
# -----------------------------------------------------------------------------

function log {
  echo "${1}" | tee -a "/tmp/austin_tests.log"
}

# -----------------------------------------------------------------------------

function step {
  log "  :: ${1}"
}


# -----------------------------------------------------------------------------
# -- Assertions
# -----------------------------------------------------------------------------

IGNORE=0
FAIL=0
REPEAT=0

# -----------------------------------------------------------------------------

function ignore {
  IGNORE=1
}

# -----------------------------------------------------------------------------

function check_ignored {
  FAIL=1

  if [ $IGNORE == 1 ] && [ $REPEAT == 0 ]
  then
    log "        The test it marked as 'ignore'"
  fi
  log
  log "       Status: $status"
  log
  log "       Collected Output"
  log "       ================"
  log
  # for line in "${lines[@]}"
  # do
  #   log "       $line"
  # done
  log "$output"
  log

  if [ $IGNORE == 0 ] && [ $REPEAT == 0 ]; then false; fi
}

# -----------------------------------------------------------------------------

function assert {
  local message="${1}"
  local condition="${2}"

  if ! eval "[[ $condition ]]"
  then
    log "      Assertion failed:  \"${message}\""
    check_ignored
  fi

  true
}

# -----------------------------------------------------------------------------

function assert_status {
  local estatus="${1}"
  : "${output?}"
  : "${status?}"

  assert "Got expected status (E: $estatus, G: $status)" "$status == $estatus"
}

# -----------------------------------------------------------------------------

function assert_success {
  : "${output?}"
  : "${status?}"

  assert "Command was successful" "$status == 0"
}

# -----------------------------------------------------------------------------

function assert_output {
  local pattern="${1}"
  : "${output?}"

  if ! echo "$output" | grep -q "${pattern}"
  then
    log "      Assertion failed:  Output contains pattern '${pattern}'"
    check_ignored
  fi

  true
}

# -----------------------------------------------------------------------------

function assert_not_output {
  local pattern="${1}"
  : "${output?}"

  if echo "$output" | grep -q "${pattern}"
  then
    log "      Assertion failed:  Output does not contain pattern '${pattern}'"
    check_ignored
  fi

  true
}

# -----------------------------------------------------------------------------

function assert_file {
  local file="$1"
  local pattern="${2}"

  if ! cat "$file" | grep -q "${pattern}"
  then
    log "      Assertion failed:  File $file contains pattern '${pattern}'"
    log
    log "File content"
    log "============"
    log
    log "$( head "$file" )"
    log ". . ."
    log "$( tail "$file" )"
    log
    check_ignored
  fi

  true
}

# -----------------------------------------------------------------------------

function assert_not_file {
  local file="$1"
  local pattern="${2}"

  if ! test -f $file
  then
    log "      Assertion failed:  File $file does not exist"
    check_ignored
  fi

  if cat "$file" | grep -q "${pattern}"
  then
    log "      Assertion failed:  File $file does not contain pattern '${pattern}'"
    log
    log "File content"
    log "============"
    log
    log "$( head "$file" )"
    log ". . ."
    log "$( tail "$file" )"
    log
    check_ignored
  fi

  true
}

# -----------------------------------------------------------------------------

function repeat {
  local times="${1}"
  shift

  REPEAT=1

  for ((i=1;i<=times;i++))
  do
    log ">> Attempt $i of $times"
    FAIL=0
    $@
    if [ $FAIL == 0 ]; then return; fi
  done

  REPEAT=0

  log "<< Test failed on $times attempt(s)."

  if [ $IGNORE == 1 ]
  then
    skip "Failed but marked as 'ignore'."
  fi

  false
}
