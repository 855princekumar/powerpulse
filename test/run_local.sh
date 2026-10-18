#!/bin/sh
# Runs the PowerPulse and SenseHive tests in a throwaway Ubuntu container, the
# same way CI does. From the folder holding both clones:
#   docker run --rm -v "$PWD/powerpulse:/pp:ro" -v "$PWD/sense-hive:/sh:ro" ubuntu:24.04 sh /pp/test/run_local.sh
set -e
export DEBIAN_FRONTEND=noninteractive
for i in 1 2 3; do                      # mirrors are sometimes slow: retry, and show the error if it still fails
  apt-get -qq update >/tmp/apt.log 2>&1 &&     apt-get -qq install -y python3 python3-venv gcc libc6-dev libmbedtls-dev mosquitto >>/tmp/apt.log 2>&1 && break
  [ "$i" = 3 ] && { tail -20 /tmp/apt.log; exit 1; }
  sleep 10
done
mosquitto -p 1883 -d                     # 2.x: anonymous clients on localhost only, which is all we need
cc -O2 -Wall -Wextra -Werror -o /tmp/seal_cli /pp/test/host/seal_cli.c -lmbedcrypto
python3 -m venv /tmp/venv
/tmp/venv/bin/pip -q install -r /sh/version-1.0/requirements.txt -r /pp/test/requirements.txt
cd /tmp
SENSEHIVE_DIR=/sh/version-1.0 SEAL_CLI=/tmp/seal_cli PP_TEST_BROKER=localhost:1883 CI=${CI:-1} \
  /tmp/venv/bin/python -m pytest -v -p no:cacheprovider /pp/test "$@"
