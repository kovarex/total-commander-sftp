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
python3 "$ROOT/tests/lagproxy.py" 2200 2222 55 8 100 & PROXY=$!
trap 'kill $PROXY; [ -f "$WORK/sshd.pid" ] && kill "$(cat "$WORK/sshd.pid")"' EXIT
for i in $(seq 1 500); do : > "$WORK/many/f$i.txt"; done
head -c 10000000 /dev/urandom > "$WORK/f10m"
# printf, not echo: dash's echo would turn "\t" in the paths into tabs
w() { printf 'Z:%s' "$(printf '%s' "$1" | tr / '\\')"; }
WKEY=$(w "$WORK/key")
printf '[test]\r\nhost=127.0.0.1\r\nport=2200\r\nuser=tcsftptest\r\nkeyfile=%s\r\n' "$WKEY" > "$WORK/ini/tcsftp.ini"
sleep 1
WINEDEBUG=-all wine "$ROOT/build/harness.exe" "$(w "$ROOT/build/tcsftp.wfx64")" "$(w "$WORK/ini")" test \
	"$(printf '%s' "$WORK" | tr / '\\')" "$(w "$WORK/f10m")"
