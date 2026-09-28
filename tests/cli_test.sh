#!/bin/sh
# Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#
# Runs the real programs. The release build is run as FernSDR runs it; it
# talks to SDRplay's API if the machine has one, and the checks accept
# either. The test build is run against the fake API.
# Usage: cli_test.sh path/to/fern-sdrplay path/to/test/fern-sdrplay path/to/libfake_sdrplay_api.so

set -u
bin=${1:?usage: cli_test.sh release-binary test-binary fake-library}
testbin=${2:?usage: cli_test.sh release-binary test-binary fake-library}
fake=${3:?usage: cli_test.sh release-binary test-binary fake-library}
for v in bin testbin fake; do
    eval "p=\$$v"
    case $p in /*) ;; *) eval "$v=\$(pwd)/\$p" ;; esac
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

pass() { echo "ok   $*" >&2; }
fail() { echo "FAIL $*" >&2; failures=$((failures + 1)); }
check() { # check <description> <command...>
    what=$1
    shift
    if "$@"; then pass "$what"; else fail "$what"; fi
}

# A directory like /sys/bus/usb/devices with two SDRplay devices.
mkdir -p "$tmp/usb/1-1" "$tmp/usb/1-2" "$tmp/usb/usb1"
echo 1df7 >"$tmp/usb/1-1/idVendor"
echo 1df7 >"$tmp/usb/1-2/idVendor"
echo 1d6b >"$tmp/usb/usb1/idVendor"

# Runs the test build the way FernSDR runs a module, against the fake API
# configured by $1: fixed environment, commands from stdin, samples, log and
# events into files.
module() {
    env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 FERNSDR_MODULE_API=1 \
        FERN_SDRPLAY_LIBRARY="$fake" FERN_SDRPLAY_USB_DIR="$tmp/usb" FAKE_SDRPLAY="$1" \
        FERN_SDRPLAY_OPEN_TIMEOUT_MS="${OPEN_TIMEOUT_MS:-12000}" \
        "$testbin" --fernsdr-module 1 >"$tmp/samples" 2>"$tmp/log" 3>"$tmp/events"
}

# Checks the event lines: argument 1 is a Python expression over the list
# `events` of parsed lines. The expressions are the literals in this script;
# nothing the module writes is evaluated.
events_are() {
    python3 - "$tmp/events" "$1" <<'PY'
import json, sys
lines = open(sys.argv[1], encoding="utf-8").read().split("\n")
assert lines[-1] == "", "the last event line has no newline"
events = [json.loads(line) for line in lines[:-1]]
hello = {"type": "hello", "api": 1, "id": "sdrplay", "kind": "input"}
assert events and all(events[0].get(k) == v for k, v in hello.items()), "no hello first: %r" % events[:1]
if not eval(sys.argv[2]):
    sys.exit("unexpected events: %r" % events)
PY
}

"$bin" --describe </dev/null >"$tmp/describe.json" 2>"$tmp/stderr"
check "--describe exits 0" test $? -eq 0
check "--describe prints valid JSON" python3 -m json.tool "$tmp/describe.json" >/dev/null
check "--describe prints one line" test "$(wc -l <"$tmp/describe.json")" -eq 1
check "--describe prints nothing on stderr" test ! -s "$tmp/stderr"
check "--describe names the module and its settings" python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert (d["api"], d["id"], d["kind"]) == (1, "sdrplay", "input"), d
assert [s["key"] for s in d["settings"]] == ["device", "antenna", "gain", "lna_state", "if_gain_reduction",
    "bias_tee", "rf_notch", "dab_notch", "am_notch", "hdr", "ppm", "dc_correction", "iq_correction", "if_mode",
    "bandwidth"]
' "$tmp/describe.json"

# The release build, with or without SDRplay's API on this machine, and
# deaf to the test build's environment variables.
FERN_SDRPLAY_LIBRARY="$fake" FAKE_SDRPLAY="devices=255:FAKE" "$bin" --list-devices </dev/null >"$tmp/list.json" 2>/dev/null
status=$?
check "--list-devices exits 0 (got $status)" test "$status" -eq 0
check "--list-devices prints a device list or says why not, and ignores FERN_SDRPLAY_LIBRARY" python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert isinstance(d["devices"], list), d
assert "error" in d or "not listed" in d["note"], d
assert "FAKE" not in json.dumps(d), d
' "$tmp/list.json"

env -i FERN_SDRPLAY_LIBRARY="$fake" FERN_SDRPLAY_USB_DIR="$tmp/usb" FAKE_SDRPLAY="devices=255:AAA,4:BBB;busy=BBB" \
    "$testbin" --list-devices </dev/null >"$tmp/list.json" 2>/dev/null
check "--list-devices lists what the API offers and counts the RSPs in use elsewhere" python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert [(x["name"], x["serial"], x["usable"]) for x in d["devices"]] == [("RSP1A", "AAA", True)], d
assert d["on_usb"] == 2 and d["in_use_elsewhere"] == 1, d
assert "not listed" in d["note"], d
' "$tmp/list.json"

env -i FERN_SDRPLAY_LIBRARY=/nonexistent/libsdrplay_api.so.3 "$testbin" --list-devices </dev/null >"$tmp/list.json" 2>/dev/null
check "--list-devices without the API says where to get it" python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
assert d["devices"] == [] and "https://www.sdrplay.com/api/" in d["error"], d
' "$tmp/list.json"

for args in "" "--bogus" "describe" "--describe --list-devices" "--describe extra" "--fernsdr-module" \
    "--fernsdr-module 2" "--fernsdr-module one" "--fernsdr-module 1 extra"; do
    # shellcheck disable=SC2086
    "$bin" $args </dev/null >"$tmp/stdout" 2>/dev/null
    status=$?
    check "'$args' exits 2 (got $status)" test "$status" -eq 2
    check "'$args' prints nothing on stdout" test ! -s "$tmp/stdout"
done

"$bin" --fernsdr-module 1 </dev/null >/dev/null 2>"$tmp/stderr" 3>&-
status=$?
check "--fernsdr-module 1 without fd 3 exits 2 (got $status)" test "$status" -eq 2
check "and says why" grep -q "fd 3 is not open" "$tmp/stderr"

"$bin" --notices </dev/null >"$tmp/notices" 2>/dev/null
check "--notices exits 0" test $? -eq 0
check "--notices states the licence exception and where the API comes from" sh -c 'grep -q "additional permission" "$1" &&
    grep -q "https://www.sdrplay.com/api/" "$1"' - "$tmp/notices"
check "--version prints the version" sh -c '"$1" --version | grep -q "^fern-sdrplay [0-9]"' - "$bin"

open='{"type":"open","sample_rate":2000000,"center":7100000,"signal":"iq","settings":{"device":"serial:AAA"}}'

# An RSP through the fake API: hello, ready, samples; EOF stops it.
{ echo "$open"; sleep 1.6; } | module "devices=255:AAA,4:BBB"
status=$?
check "open, samples and EOF exit 0 (got $status)" test "$status" -eq 0
check "ready announces the band" events_are \
    'events[1]["type"] == "ready" and events[1]["sample_rate"] == 2000000 and events[1]["center"] == 7100000 and events[1]["format"] == "s16" and events[1]["signal"] == "iq" and events[1]["device"]["serial"] == "AAA"'
check "stats follow" events_are 'any(e["type"] == "stats" and e["samples"] > 0 for e in events)'
check "samples arrived" test -s "$tmp/samples"
check "samples are whole I/Q pairs" sh -c 'test $(( $(wc -c <"$1") % 4 )) -eq 0' - "$tmp/samples"
check "log lines are at most 1 KiB" awk 'length($0) > 1023 { exit 1 }' "$tmp/log"

# Two RSPs, none chosen.
echo '{"type":"open","sample_rate":2000000,"center":7100000,"signal":"iq"}' | module "devices=255:AAA,4:BBB"
status=$?
check "two RSPs and no device setting exit 3 (got $status)" test "$status" -eq 3
check "and name both" events_are 'events[1]["code"] == "no-device" and "AAA" in events[1]["message"] and "BBB" in events[1]["message"]'
check "and write no samples" test ! -s "$tmp/samples"

# The chosen one is held by another process.
echo "$open" | module "devices=255:AAA,4:BBB;busy=AAA"
status=$?
check "an RSP in use elsewhere exits 4 (got $status)" test "$status" -eq 4
check "and says so" events_are 'events[1]["code"] == "busy" and events[1]["fatal"] is True'

# An API version whose layout the module does not know.
echo "$open" | module "devices=255:AAA;version=3.16"
status=$?
check "an unknown API version exits 1 (got $status)" test "$status" -eq 1
check "and names it" events_are '"3.16" in events[1]["message"]'

# The service is not running.
echo "$open" | module "devices=255:AAA;open_error=1"
status=$?
check "a stopped service exits 1 (got $status)" test "$status" -eq 1
check "and says how to start it" events_are '"systemctl start sdrplay" in events[1]["message"]'

# A call that hangs ends the process with an error, within the deadline.
start=$(date +%s)
echo "$open" | OPEN_TIMEOUT_MS=500 module "devices=255:AAA;lock_ms=4000"
status=$?
took=$(( $(date +%s) - start ))
check "a hung API call exits 4 (got $status)" test "$status" -eq 4
check "within the open deadline, not the call's 4 s (took ${took} s)" test "$took" -le 3
check "and says which call" events_are 'events[1]["code"] == "busy" and "sdrplay_api_LockDeviceApi" in events[1]["message"]'

# No API at all.
env -i PATH=/usr/bin:/bin FERN_SDRPLAY_LIBRARY=/nonexistent/libsdrplay_api.so.3 \
    "$testbin" --fernsdr-module 1 >"$tmp/samples" 2>"$tmp/log" 3>"$tmp/events" <<EOF2
$open
EOF2
status=$?
check "no API installed exits 1 (got $status)" test "$status" -eq 1
check "and says where to get it" events_are '"https://www.sdrplay.com/api/" in events[1]["message"]'

# Invalid settings are refused before the API is touched.
echo '{"type":"open","sample_rate":2000000,"center":7100000,"signal":"iq","settings":{"squelch":1}}' | module "devices=255:AAA"
status=$?
check "an unknown setting exits 6 (got $status)" test "$status" -eq 6
check "an unknown setting is reported as invalid" events_are \
    'events[1]["code"] == "invalid" and "module.squelch" in events[1]["message"]'

module "devices=255:AAA" </dev/null
status=$?
check "EOF on fd 0 exits 0 (got $status)" test "$status" -eq 0
check "EOF on fd 0: only hello" events_are 'len(events) == 1'

printf '%s\n' '{"type":"future","x":1}' 'garbage' '{"type":"stop"}' | module "devices=255:AAA"
status=$?
check "stop exits 0 (got $status)" test "$status" -eq 0

# SIGTERM while streaming. env execs the program, so $! is its pid.
mkfifo "$tmp/commands"
env -i PATH=/usr/local/bin:/usr/bin:/bin FERN_SDRPLAY_LIBRARY="$fake" FAKE_SDRPLAY="devices=255:AAA" \
    "$testbin" --fernsdr-module 1 <"$tmp/commands" >"$tmp/samples" 2>"$tmp/log" 3>"$tmp/events" &
pid=$!
exec 4>"$tmp/commands"
echo "$open" >&4
sleep 0.5
kill -TERM "$pid"
wait "$pid"
status=$?
exec 4>&-
check "SIGTERM exits 0 (got $status)" test "$status" -eq 0
check "SIGTERM: ready had been sent" events_are 'events[1]["type"] == "ready"'
check "SIGTERM: the API was closed" grep -q "stopping on a signal" "$tmp/log"

if [ "$failures" -ne 0 ]; then
    echo "cli_test: $failures failed" >&2
    exit 1
fi
echo "cli_test: all passed" >&2
