# QFtpCompat

A Qt6-compatible asynchronous FTP client library.  
Drop-in replacement for the `QFtp` class that was removed from Qt5.

**[API Documentation](https://DeiVadder.github.io/QFtpCompat/html/)**

---

## Requirements

| Dependency | Minimum version |
|---|---|
| Qt | 6.2 |
| CMake | 3.21 |
| C++ standard | 17 |

---

## Integration

### As a CMake subdirectory (recommended)

```cmake
add_subdirectory(3rdParty/QFtpCompat)
target_link_libraries(MyApp PRIVATE QFtpCompat::QFtpCompat)
```

```cpp
#include <QFtpCompat>
```

`Qt6::Core` and `Qt6::Network` are propagated automatically via the
`QFtpCompat::QFtpCompat` target – no extra `find_package` needed in
consuming projects.

---

## Quick start

```cpp
#include <QFtpCompat>

auto *ftp = new QFtpCompat(this);

connect(ftp, &QFtpCompat::commandFinished, this,
    [ftp](int /*id*/, bool error) {
        if (error) qWarning() << "FTP error:" << ftp->errorString();
    });

connect(ftp, &QFtpCompat::listInfo, this,
    [](const QFtpCompatDirEntry &e) { qDebug() << e.toString(); });

connect(ftp, &QFtpCompat::done, this,
    [](bool error) { qDebug() << "All done, error:" << error; });

// Commands are queued and executed sequentially
ftp->open(QUrl("ftp://user:pass@192.168.1.1/"));
ftp->list(QUrl("ftp://192.168.1.1/data/"));

// Alternative: build QUrl with setters
QUrl url("ftp://192.168.1.1/");
url.setUserName("admin");
url.setPassword("secret");
ftp->open(url);

QByteArray buffer;
ftp->get(QUrl("ftp://192.168.1.1/data/config.cfg"), &buffer);
ftp->put(QUrl("ftp://192.168.1.1/data/config.cfg"), buffer);

ftp->rename(QUrl("ftp://192.168.1.1/old.txt"), QUrl("ftp://192.168.1.1/new.txt"));
ftp->remove(QUrl("ftp://192.168.1.1/old.txt"));
ftp->mkdir (QUrl("ftp://192.168.1.1/archive/"));
ftp->close();
```

---

## API overview

### Connection

| Method | Description |
|---|---|
| `open(QUrl)` | Connect and log in (user/pass from URL). |
| `close()` | Send QUIT and close the connection. |
| `abort()` | Abort the current transfer (sends ABOR). |
| `resetConnection()` | Hard-reset via TCP RST – no QUIT sent. Safe in any state. |

### Transfer

| Method | Description |
|---|---|
| `get(url, QIODevice*)` | Download to an open device. |
| `get(url, QByteArray*)` | Download into a byte array. |
| `put(url, QByteArray)` | Upload from a byte array. |
| `put(url, QIODevice*)` | Upload from an open device. |

### Directory / management

| Method | FTP command | Description |
|---|---|---|
| `list(url)` | `LIST` | Directory listing; each entry emitted via `listInfo()`. |
| `mkdir(url)` | `MKD` | Create directory. |
| `rmdir(url)` | `RMD` | Remove empty directory. |
| `remove(url)` | `DELE` | Delete file. |
| `rename(from, to)` | `RNFR`+`RNTO` | Rename or move. |
| `cd(url)` | `CWD` | Change working directory. |
| `pwd()` | `PWD` | Current directory (reply via `rawCommandReply`). |
| `rawCommand(cmd)` | — | Send any FTP command; reply via `rawCommandReply`. |

### Signals

| Signal | When |
|---|---|
| `stateChanged(State)` | Connection state changed. |
| `commandStarted(int id)` | A queued command began executing. |
| `commandFinished(int id, bool error)` | Command completed. |
| `done(bool error)` | All queued commands finished. |
| `listInfo(QFtpCompatDirEntry)` | One entry per `list()` result. |
| `dataTransferProgress(qint64, qint64)` | Bytes transferred / total. |
| `rawCommandReply(int code, QString)` | Server reply to `rawCommand()` / `pwd()`. |

---

## Transfer modes

### Connection mode

Only **Passive** (PASV) is implemented.  
Active mode (PORT) is reserved as `TransferMode::Active` for a future release.

### Transfer type

| Value | FTP command | Status |
|---|---|---|
| `TransferType::Binary` | `TYPE I` | Fully implemented. |
| `TransferType::Ascii`  | `TYPE A` | Command sent; CRLF normalisation not yet applied. |

---

## Directory entry format

`QFtpCompatDirEntry::parseList()` recognises three listing formats, tried in order:

1. **MLSD** (RFC 3659) – `Type=file;Size=12345;Modify=20210101120000; name`
2. **Unix** (`ls -l`) – `-rw-r--r-- 1 owner group 12345 Jan  1 12:00 name`
3. **Windows** (MS-DOS) – `01-01-21  12:00PM       12345 name`

---

## Testing

The `test/` directory contains a standalone Qt console application that
exercises every public API method against both a plain FTP server and an
Explicit FTPS server.

### Dependencies

```bash
pip3 install pyftpdlib pyopenssl
```

### Step 1 – Create a test directory

```bash
mkdir ~/ftproot
```

### Step 2 – Generate a self-signed certificate (once)

```bash
cd ~/ftproot
openssl req -x509 -newkey rsa:2048 \
    -keyout key.pem -out cert.pem \
    -days 365 -nodes -subj "/CN=localhost"
```

> The certificate is only used locally for testing.
> Do **not** commit `*.pem` files – they are excluded by `.gitignore`.

### Step 3 – Start both servers (two terminals)

**Terminal 1 – plain FTP (port 2121):**
```bash
cd ~/ftproot
python3 -m pyftpdlib -u root -P root -w -p 2121
```

**Terminal 2 – Explicit FTPS (port 2122):**
```bash
cd ~/ftproot
python3 /path/to/QFtpCompat/test/server_tls.py
```

Both servers use `~/ftproot` as their root directory and
`root` / `root` as credentials.

### Step 4 – Build and run the test

```bash
cd test
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt6
cmake --build .
./FtpCompatTest
```

> **TLS server not available?**
> The test still runs – plain FTP completes normally, then the FTPS
> section reports `[FAIL] OPEN (FTPS) connection refused` and exits
> cleanly without hanging.

### Coverage

#### Plain FTP (port 2121)

| Test case | What is verified |
|---|---|
| `open()` | Login with credentials in `QUrl` |
| `put(QByteArray)` | Upload from memory |
| `put(QIODevice*)` | Upload 128 KB file with chunked progress |
| `mkdir()` / `rmdir()` | Directory lifecycle |
| `list()` root + subdir | Entry count and file visibility |
| `get(QByteArray*)` | Download + byte-exact content comparison |
| `get(QIODevice*)` | Download 128 KB to `QFile`, size verified |
| `rename()` | `RNFR`/`RNTO` round-trip |
| `remove()` | Delete all test files |
| `pwd()` / `rawCommand()` | Raw command path |
| `get()` non-existent file | 550 error received as expected |
| Final `list()` | Server root empty – full cleanup confirmed |

#### Explicit FTPS (port 2122)

| Test case | What is verified |
|---|---|
| `open(..., Explicit)` | `AUTH TLS` → `PBSZ 0` → `PROT P` → login |
| Self-signed certificate | `sslErrors → ignoreSslErrors` accepted |
| `put()` over TLS | Upload on encrypted data channel |
| `list()` | File visible in encrypted directory listing |
| `get()` over TLS | Download + byte-exact content comparison |
| `remove()` + final `list()` | Cleanup confirmed |

### Expected output (both servers running)

```
── CONNECT (plain)
  [OK  ] OPEN
  [OK  ] UPLOAD small / large / nested
  ...
  [OK  ] LIST final   → 0 entries (clean ✓)
══ Plain FTP complete ══

══════════════════════════════════════════
 Explicit FTPS test (port 2122)
══════════════════════════════════════════
── CONNECT (Explicit FTPS – AUTH TLS)
  [OK  ] OPEN (FTPS)
  [OK  ] UPLOAD         → encrypted data channel
  [OK  ] LIST           → tls_test.txt visible ✓
  [OK  ] DOWNLOAD       → content match ✓
  [OK  ] DELETE
  [OK  ] LIST final     → 0 entries (clean ✓)
══ Explicit FTPS complete ══
```

---

## Building the documentation

```bash
cmake -DQFTPCOMPAT_BUILD_DOCS=ON ..
cmake --build . --target QFtpCompat_docs

# Open immediately
cmake --build . --target QFtpCompat_open_docs
```

The generated HTML is copied to `docs/html/` automatically, ready for
GitHub Pages (see below).

Requires [Doxygen](https://www.doxygen.nl).  
[Graphviz](https://graphviz.org) (`dot`) is optional but enables class diagrams.

### Publishing to GitHub Pages

1. Push the repository to GitHub.
2. **Settings → Pages → Source**: branch `main`, folder `/docs`.
3. Save – your docs will appear at:

```
https://YOUR_GITHUB_USERNAME.github.io/QFtpCompat/html/
```

Update the link at the top of this README to match.

---

## License

MIT – see [LICENSE](LICENSE).
