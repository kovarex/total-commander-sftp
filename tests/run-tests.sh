#!/bin/sh
# End-to-end test: local sshd behind a proxy that adds 55 ms each way and caps
# upload at 8 Mbit/s, then the plugin is driven under Wine by tests/harness.exe.
# Needs root (creates a test user), openssh-server, python3 and wine64.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/tmp/tcsftp-test}
rm -rf "$WORK" && mkdir -p "$WORK/ini" "$WORK/many"
id tcsftptest >/dev/null 2>&1 || useradd -m tcsftptest
mkdir -p /run/sshd ~tcsftptest/.ssh
ssh-keygen -A >/dev/null
ssh-keygen -q -t ed25519 -N "" -f "$WORK/key"
cp "$WORK/key.pub" ~tcsftptest/.ssh/authorized_keys
chown -R tcsftptest ~tcsftptest/.ssh && chmod 700 ~tcsftptest/.ssh
chmod 777 "$WORK"
/usr/sbin/sshd -p 2222 -o ListenAddress=127.0.0.1 -o PidFile="$WORK/sshd.pid"
# the first connection is cut 5 MB into the upload, so the test also covers reconnect + resume
python3 "$ROOT/tests/lagproxy.py" 2200 2222 55 8 100 5000000 & PROXY=$!
# a line that freezes every 1.5 MB, so one upload needs many reconnects in a row
python3 "$ROOT/tests/lagproxy.py" 2201 2222 55 8 100 1500000 stall & STALLPROXY=$!
# a second sshd whose sftp-server can't write past 3 MB (EFBIG -> generic sftp failure, status 4),
# like a server that refuses the same spot of a file on every attempt
printf '#!/bin/sh\ntrap "" XFSZ\nulimit -f 6000\nexec /usr/lib/openssh/sftp-server\n' > "$WORK/sftp-limited"
chmod 755 "$WORK/sftp-limited"
/usr/sbin/sshd -p 2223 -o ListenAddress=127.0.0.1 -o PidFile="$WORK/sshd-limited.pid" -o "Subsystem=sftp $WORK/sftp-limited"
trap 'kill $PROXY $STALLPROXY; for p in "$WORK"/sshd*.pid; do [ -f "$p" ] && kill "$(cat "$p")"; done' EXIT
for i in $(seq 1 500); do : > "$WORK/many/f$i.txt"; done
head -c 10000000 /dev/urandom > "$WORK/f10m"
# printf, not echo: dash's echo would turn "\t" in the paths into tabs
w() { printf 'Z:%s' "$(printf '%s' "$1" | tr / '\\')"; }
WKEY=$(w "$WORK/key")
printf '[test]\r\nhost=127.0.0.1\r\nport=2200\r\nuser=tcsftptest\r\nkeyfile=%s\r\n' "$WKEY" > "$WORK/ini/tcsftp.ini"
sleep 1
WINEDEBUG=-all wine "$ROOT/build/harness.exe" "$(w "$ROOT/build/tcsftp.wfx64")" "$(w "$WORK/ini")" test \
	"$(printf '%s' "$WORK" | tr / '\\')" "$(w "$WORK/f10m")" "$WORK/f10m"

echo "--- upload over a line that stalls every 1.5 MB"
mkdir -p "$WORK/ini-stall"
printf '[General]\r\nTimeoutSeconds=3\r\n[stall]\r\nhost=127.0.0.1\r\nport=2201\r\nuser=tcsftptest\r\nkeyfile=%s\r\n' "$WKEY" > "$WORK/ini-stall/tcsftp.ini"
cp "$WORK/ini/tcsftp_known_hosts" "$WORK/ini-stall/" 2>/dev/null || true
UPLOAD_ONLY=1 WINEDEBUG=-all wine "$ROOT/build/harness.exe" "$(w "$ROOT/build/tcsftp.wfx64")" "$(w "$WORK/ini-stall")" stall \
	"$(printf '%s' "$WORK" | tr / '\\')" "$(w "$WORK/f10m")" "$WORK/f10m" | tee "$WORK/stall.out"
RECONNECTS=$(grep -c "Reconnecting and resuming" "$WORK/stall.out" || true)
if cmp -s "$WORK/wine-up.bin" "$WORK/f10m" && [ "$RECONNECTS" -gt 5 ]; then
	echo "PASS upload finished identical after $RECONNECTS reconnects"
else
	echo "FAIL upload over stalling line ($RECONNECTS reconnects)"; exit 1
fi

echo "--- upload the server refuses at the same offset every time"
mkdir -p "$WORK/ini-refuse"
printf '[General]\r\nTransferRetries=3\r\n[refuse]\r\nhost=127.0.0.1\r\nport=2223\r\nuser=tcsftptest\r\nkeyfile=%s\r\n' "$WKEY" > "$WORK/ini-refuse/tcsftp.ini"
cp "$WORK/ini/tcsftp_known_hosts" "$WORK/ini-refuse/" 2>/dev/null || true
EXPECT_UPLOAD_FAIL=1 WINEDEBUG=-all timeout 300 wine "$ROOT/build/harness.exe" "$(w "$ROOT/build/tcsftp.wfx64")" "$(w "$WORK/ini-refuse")" refuse \
	"$(printf '%s' "$WORK" | tr / '\\')" "$(w "$WORK/f10m")" "$WORK/f10m" | tee "$WORK/refuse.out"
RECONNECTS=$(grep -c "Reconnecting and resuming" "$WORK/refuse.out" || true)
if grep -q "^PASS upload refused" "$WORK/refuse.out" && [ "$RECONNECTS" -le 4 ]; then
	echo "PASS refused upload gave up after $RECONNECTS reconnects"
else
	echo "FAIL refused upload ($RECONNECTS reconnects)"; exit 1
fi
