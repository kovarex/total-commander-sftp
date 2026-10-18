# tc-sftp

An open-source SFTP file system plugin for [Total Commander](https://www.ghisler.com/) (64-bit), focused on speed over high-latency links.

## Why

SFTP sends file data in small chunks and the server acknowledges each one. On a connection with a long round trip (for example Europe to a US server, ~110 ms), throughput depends on how much data the client keeps "in flight" while it waits for acknowledgements. Total Commander's bundled SFTP plugin writes a block, waits until the whole block is acknowledged, then reads the next one, so the pipe drains at every block boundary.

tc-sftp keeps a sliding window of data (2 MB by default) in flight for the whole transfer, the same way OpenSSH's `sftp` and WinSCP do.

Measured on a simulated link with 110 ms round trip:

| Upload speed | Line 8 Mbit/s | Line 20 Mbit/s |
|---|---|---|
| Block-and-wait loop (like the bundled plugin) | 6.4 Mbit/s | 13.9 Mbit/s |
| tc-sftp | 7.7 Mbit/s | 19.5 Mbit/s |

Other round-trip savings:

- Uploading a new file uses an exclusive create, so there is no separate "does it exist?" request per file.
- No fixed sleeps in the request loops; every wait is on the socket.
- `TCP_NODELAY` on the connection.
- Copy within the server (Shift+F5 inside the plugin) runs `cp` on the server instead of sending the data through your PC.

## Install

Open `tcsftp.zip` in Total Commander (Enter on the file) and confirm the plugin installation. It appears under **Network Neighborhood** as **tc-sftp**.

## Use

- **&lt;Quick connection&gt;**: type `user@host:port/path` and connect without saving.
- **&lt;New connection&gt;**: asks for a name, server, optional private key file and start directory, then saves it.
- Saved connections: Enter connects. **Alt+Enter** edits them. **F8** deletes them, and **F6** renames them.
- Login order: the key file you set, otherwise Pageant, the Windows OpenSSH agent and `%USERPROFILE%\.ssh\id_ed25519`/`id_ecdsa`/`id_rsa`. After that it tries a password or keyboard-interactive login. Passwords can be kept in Total Commander's encrypted password store (master password).
- Server keys are checked against `tcsftp_known_hosts` (OpenSSH format) next to `wincmd.ini`. You are asked on first connection and warned if a key changes.
- Commands typed in Total Commander's command line run on the server in the current directory, and their output is shown.
- Unix permissions are shown and can be changed with **Files → Change Attributes**.
- Background transfers (the **Background** button in the copy dialog) use a separate connection.
- If the connection drops or freezes during a transfer, the plugin reconnects and continues where the data ended, as many times as needed while data keeps moving. Failures and retries are written with their reason to Total Commander's log window and to `tcsftp.log` next to `wincmd.ini`.

## Settings

Connections and settings live in `tcsftp.ini` next to `wincmd.ini`. Optional `[General]` keys:

| Key | Default | Meaning |
|---|---|---|
| `WindowKB` | 2048 | Data kept in flight per transfer (KB). Raise it for fast links with long round trips. |
| `TimeoutSeconds` | 30 | How long the connection may go silent before the plugin treats it as dead and reconnects. |
| `TransferRetries` | 10 | A transfer keeps reconnecting and resuming as long as each attempt moves data. It gives up after this many attempts in a row that move nothing. |
| `FileMode` | 644 | Permissions for new files (octal). |
| `DirMode` | 755 | Permissions for new directories (octal). |

## Build

Cross-compiled on Linux with mingw-w64. OpenSSL and libssh2 are linked statically, so the plugin is a single file.

```sh
sudo apt install mingw-w64 cmake zip
scripts/build-deps.sh   # builds OpenSSL 3.0 and libssh2 1.11.1 into ./deps
./build.sh              # produces build/tcsftp.wfx64 and build/tcsftp.zip
```

### Tests

`tests/run-tests.sh` starts a local sshd behind a proxy that adds 55 ms each way and caps upload at 8 Mbit/s. It then drives the plugin through its Total Commander entry points under Wine. Needs root, `openssh-server`, `python3` and `wine64`.

## License

MIT. Uses [libssh2](https://libssh2.org/) (BSD-3-Clause) and [OpenSSL](https://www.openssl.org/) (Apache-2.0).
