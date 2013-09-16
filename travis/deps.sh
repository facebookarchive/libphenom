#!/bin/sh
set -x
case `uname` in
  Linux)
    sudo apt-get update -y -q
    sudo apt-get install -y valgrind -q
    sudo apt-get install -y libc-ares-dev -q
    ;;
esac
