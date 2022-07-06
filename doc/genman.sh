#!/bin/bash

help2man \
    -n "Frame stack sampler for CPython" \
    -i doc/examples.troff \
    src/austin > src/austin.1
