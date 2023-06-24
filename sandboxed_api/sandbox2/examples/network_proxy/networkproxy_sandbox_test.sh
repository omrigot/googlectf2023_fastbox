#!/bin/bash
#
# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

die() {
  echo "$1" 1>&2
  exit 1
}

[[ -n "$COVERAGE" ]] && exit 0

# Find input files
BINDIR=$TEST_SRCDIR/com_google_sandboxed_api/sandboxed_api/sandbox2
EXE=$BINDIR/examples/network_proxy/networkproxy_sandbox

# test it
ls "${EXE}" || exit 2

"${EXE}" --connect_with_handler || die 'TEST1 FAILED: it should have exited with 0'
"${EXE}" --noconnect_with_handler || \
    die 'TEST2 FAILED: it should have exited with 0'

exit 0
