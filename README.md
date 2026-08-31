# ps5-dl-filemanager-payload

Unified high-performance **PS5 `.elf` payload** for the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) toolchain (FreeBSD 12 / Prospero, AMD Zen 2).

One payload, four engines:

| Module | File | What it does |
|---|---|---|
| **Download Manager** | `src/downloader.c` | Multi-connection HTTP/1.1 engine — chunked `Range` requests over non-blocking sockets + `poll()`, memory-buffered `pwrite()` flushes, `.p5part` sidecar for **resume/pause**, pluggable TLS transport hook |
| **File Manager Core** | `src/filemanager.c` | Async worker-pool queue for copy/move/delete/rename across PS5 mount points (`/user`, `/mnt/usb0`, …); same-device moves use `rename(2)`, cross-mount falls back to stream-copy + delete |
| **UnRAR Engine** | `src/unrar_engine.cpp` | Multi-threaded extraction over the libunrar C ABI; producer/consumer pipeline overlaps Zen 2 decompression with disk writes, path-traversal (zip-slip) hardened |
| **FFPFSC Converter** | `src/ffpfsc.c` | Packs a folder tree into a flat `.ffpfsc` container — 4K-aligned blobs + FNV-1a-64 checksummed index, written through a 16 MiB bounce buffer for max sequential disk throughput |

## Repository layout

```
├── .github/workflows/build.yml   # CI: compiles payload.elf on every push
├── Makefile                      # prospero clang toolchain build
├── include/
│   ├── common.h                  # error codes, memory caps, logging, timing
│   ├── downloader.h
│   ├── filemanager.h
│   ├── unrar_engine.h
│   └── ffpfsc.h
└── src/
    ├── main.c                    # entry point + command dispatcher
    ├── downloader.c
    ├── filemanager.c
    ├── unrar_engine.cpp
    └── ffpfsc.c
```

## Build

### GitHub Actions (CI)
Every push to `main` builds inside the official SDK container
`ghcr.io/ps5-payload-dev/sdk:latest` and uploads `payload.elf` as an
artifact. Tag a commit to publish a GitHub Release with the ELF attached.

### Local

```sh
git clone https://github.com/ps5-payload-dev/sdk
cd sdk && ./install.sh          # installs prospero clang toolchain
cd ..
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make -j8
# -> payload.elf
```

## Usage

Load `payload.elf` with a payload loader (e.g. ps5-payload-dev `elfldr`).

**One-shot mode** — arguments dispatched directly:

```sh
payload.elf dl http://host/file.pkg /user/downloads/file.pkg 4
payload.elf unrar /user/downloads/game.rar /user/extracted
payload.elf pack /user/extracted/game /mnt/usb0/game.ffpfsc
payload.elf verify /mnt/usb0/game.ffpfsc
```

**Resident mode** — no arguments; reads line-based commands from stdin
(loader-socket stdio):

```
dl <url> <dest> [connections]    multi-connection download (resume-capable)
ls <path>                        directory listing
cp|mv|rn <src> <dst>             async copy / move / rename
rm <path>                        recursive delete
unrar <archive> <dir>            multi-threaded extraction
pack <dir> <out.ffpfsc>          folder -> container
verify <file.ffpfsc>             integrity check
exit
```

## Design notes

- **Memory safety**: hard caps per module — 32 MiB per download connection,
  8 MiB copy buffer, 4 MiB unrar I/O, 16 MiB ffpfsc write buffer — keeping
  total payload footprint well under PS5 payload constraints.
- **Threading**: default 4 download connections and `cores-1` unrar writers,
  leaving headroom on the 8-core Zen 2 for the system.
- **Failure discipline**: partial downloads resume from a magic-verified
  sidecar; failed copies never leave partial destination files; failed packs
  delete the output container.
- **HTTPS**: the downloader exposes `p5_dl_set_transport_factory()` — install
  a TLS transport (e.g. mbedTLS-backed) without touching the scheduler.

## License

MIT — see [LICENSE](LICENSE).

## Disclaimer

For homebrew research and development on consoles you own. Not affiliated
with Sony Interactive Entertainment.
