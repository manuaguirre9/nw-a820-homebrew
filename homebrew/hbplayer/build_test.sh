#!/bin/sh
# the on-device test build: scripted keys + screenshots + multi-file selftest (never shipped; see tests/device)
cd "$(dirname "$0")"
OUT=hbtest EXTRA=-DHB_DEV sh build.sh
