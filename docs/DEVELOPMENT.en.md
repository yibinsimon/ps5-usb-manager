# PS5 USB Manager — developer docs

Changing code, building the payload, running the tests, getting the ELF onto the console, on-device acceptance — all of that is in this file.
If you just want to use the tool, read [README.en.md](../README.en.md) instead.

> Back to [README.en.md](../README.en.md) ｜ [中文开发文档](DEVELOPMENT.md)

## Contents

- [Building the payload](#building-the-payload)
- [Tests](#tests)
- [On-device acceptance](#on-device-acceptance)
- [Deploying to the console](#deploying-to-the-console)
- [API](#api)
- [Single instance (repeated load and takeover)](#single-instance-repeated-load-and-takeover)
- [Volume labels](#volume-labels)
- [App icon](#app-icon)
- [Safety model](#safety-model)
- [Naming](#naming)
- [Design notes (selected)](#design-notes-selected)
- [Repository layout](#repository-layout)

---

## Building the payload

You need `ps5-payload-sdk` plus a host `clang` / `ld.lld`. On Windows, llvm-mingw is enough — **no WSL, no Docker**, because the SDK release ships its own Windows toolchain (`win/prospero-lld.exe`, `win/ninja.exe`).

```bash
cd device/ps5-usbmanage
bash build.sh
```

The script reads the environment first, then looks for the SDK under `$HOME`, `/opt` and parent directories. If it finds nothing it fails loudly and prints how to set things up — it will not silently fall back to a hard-coded path.

```bash
export PS5_PAYLOAD_SDK="$HOME/ps5-payload-sdk"        # the unpacked SDK root
export PS5_CLANG="/c/llvm-mingw/bin/clang.exe"        # if unset, clang is taken from PATH
```

The script generates the embedded page array, compiles, then runs a round of self-checks: ELF header, `NEEDED`, dynamic symbols, whether the page and the icon really made it into the binary, whether the names are embedded. Any failure aborts the build.

```bash
# Linux / macOS
sudo apt-get install clang-18 lld-18     # Debian family; or brew install llvm lld
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

The build converts `web/index.html` to `web/index_html.h` with `xxd` before compiling, so `xxd` is required (it comes with vim-common). If it is missing the build fails outright rather than silently producing an ELF with no page in it.

`win\build.cmd` is the cmd entry point for the same build: if `bash` is on PATH it simply forwards to `build.sh`, otherwise it degrades to calling clang from plain cmd.

### Three pitfalls we already hit (handled in the script; mind them when changing environment)

1. **Do not use the SDK's own `win/prospero-clang.cmd`.** The POSIX `bin/prospero-clang` blanks out `crt1.o` for clang major ≥ 20, because from clang 20 the driver appends the crt objects itself; the `.cmd` version lacks that check and still passes `crt1.o` by absolute path, so linking always fails with `duplicate symbol: payload_exit`. Our script therefore never passes `crt1.o` explicitly.
2. **Under Git Bash, PATH entries must be POSIX form (`/c/...`).** Putting `C:/...` there breaks MSYS path translation and clang reports `unable to execute command: program not executable`.
3. **PATH must contain both the clang directory and `SDK/win`.** `prospero-lld.exe` is only a wrapper that calls `ld.lld` internally — the former lives in `SDK/win`, the latter next to clang. Also worth setting `SCE_PROSPERO_SDK_DIR` so the driver points `--sysroot` at the SDK itself.

### Key flags

The same as the official `samples/mntinfo`:

```bash
clang --start-no-unused-arguments \
  -target x86_64-sie-ps5 \
  -fvisibility-nodllstorageclass=default \
  -isysroot "$SDK" -isystem "$SDK/target/include" \
  -L "$SDK/target/lib" -L "$SDK/target/user/homebrew/lib" \
  -fno-stack-protector -fno-plt -femulated-tls \
  -lc -lkernel_sys \
  --end-no-unused-arguments \
  usbmanage.c -o usbmanage.elf \
  --start-no-unused-arguments --sysroot "$SDK" \
  -lSceLibcInternal -lSceNet \
  --end-no-unused-arguments
```

`-lkernel_sys` (rather than the default `-lkernel_web`) is used because this payload calls `getfsstat()` / `unmount()` — the official mount-enumeration sample `samples/mntinfo` links exactly this way. The resulting object:

```
Class: ELF64      OS/ABI: UNIX - FreeBSD     Type: DYN (PIE)     Machine: x86-64
NEEDED: libkernel_sys / libSceLibcInternal / libSceNet / libkernel_web
        + libSceSystemService / libSceUserService / libSceNotification   (for the on-screen UI)
```

Artifacts live in `dist/`:

| File | Notes |
|---|---|
| `dist/usbmanage-v1.0.0.elf` | **the current release build** |
| `dist/SHA256SUMS.txt` | sha256 checksums, **the values inside the file are authoritative** |
| `dist/_legacy/` | development-era builds, archived locally only (not committed) |

> **Always check before releasing**: the mtime of `usbmanage.c` / `web/index.html` / `web/icon.png` must be **earlier** than `usbmanage.elf`. We once changed the source and still released the old artifact — an artifact older than its source means it was never rebuilt.

> **About the `PT_DYNAMIC` warning**: `llvm-readelf -l` reports `invalid PT_DYNAMIC size` on this artifact. It happens because the linker puts `.dynamic` / `.dynsym` / `.dynstr` / `.rela.dyn` into a single `PT_DYNAMIC` segment, and the only trigger is that the segment size is not a multiple of 16 — **the SDK's own `samples/mntinfo` reports it too** (measured: this artifact 0x1978 / 2 entries, `mntinfo-ref` 0x1018 / 2 entries, while `browser-ref` and `notify-ref` report 0). `.dynamic` terminates normally with `DT_NULL` (26 entries) and the runtime loader walks that array, unaffected by segment size. It is a harmless warning.

> **About what sha256 means here**: the linker writes a 16-byte build-id into `.note.gnu.build-id`, different on every link, so **the same source compiled twice will never produce the same sha256**. We verified this: strip that note section and the rest of the bytes are identical between two builds — semantically reproducible, only the fingerprint changes. So the sha256 here is for **verifying that a transfer or copy did not corrupt the file** (`sha256sum -c dist/SHA256SUMS.txt`), not a source-to-bytes reproducibility credential.

---

## Tests

Three layers, all on the host. No PS5 required:

```bash
cd device/ps5-usbmanage

bash hosttest/run.sh        # 139 assertions — pure logic (functions extracted, host clang)
bash hosttest/web/run.sh    #  73 assertions — page JS logic (minimal DOM, needs node)
bash hosttest/http/run.sh   # 259 assertions — HTTP end-to-end (C source built + real socket + curl)
```

| Layer | Assertions | Covers |
|---|---|---|
| `hosttest/run.sh` | 139 | Pure functions: allow-list rules, volume-label parsing, language detection, path decoding |
| `hosttest/web/run.sh` | 73 | Page rendering, language switching (Simplified / Traditional / English), `pending` feedback restore |
| `hosttest/http/run.sh` | 259 | Every endpoint, status codes, CSRF and takeover handshake, three-language `msg`, notification text |

`hosttest/http` really starts a server on the host and drives it with `curl`. It takes about 3 minutes.

### Pure-logic layer (139)

`hosttest/run.sh` uses `awk` to extract platform-independent pure functions **by name** from the `usbmanage.c` source itself (`path_allowed` / `url_decode` / `json_escape` / `room` / `path_under` / `holder_killable` / `extract_mount` / `fsboot_parse` / `utf16le_to_utf8` / `label_from_83` / `exfat_label_in_dir` / `fat_label_in_dir` / `starts_with_ci` / `lang_of` / `accept_language_lang` / `env_lang` and friends) into `pure.inc`, so the code under test is the very code that goes into the ELF, not a copy. Language types (`LANG_ZH_HANS` / `LANG_ZH_HANT` / `LANG_EN`) and the label structures are carved out of the source with explicit markers, shared by test and implementation alike. Coverage:

- Allow-list accepts: `/mnt/usb0`, `/mnt/usb1`, `/mnt/usb12`…
- Allow-list rejects: `/mnt/ext0`, `/mnt/ext1` (internal storage), `/mnt/usb`, `/mnt/usb0/`, `/mnt/usb0/../ext0`, `//mnt/usb0`, `/mnt/usbx`, NULL, empty string…
- URL decoding: `%2Fmnt%2Fusb0` → `/mnt/usb0`; invalid escapes `%ZZ`, `%2` are left as-is; the allow-list runs **after** decoding (`%2Fmnt%2Fext0` is still refused)
- `room()`: remaining-space arithmetic (`off == 0` must return the full size, or the first write is dropped)
- `path_under()`: sub-path boundaries (`/mnt/usb00` is not a child of `/mnt/usb0`)
- `holder_killable()`: the three guard rails (this process / pid ≤ 1 / `Sce*` prefix / `init` / `kernel`)
- `extract_mount()`: query strings carrying `force=1` / `confirm=1` are no longer truncated
- Boot-sector detection: the two gates `55 AA` and a leading jump byte; the exFAT `"EXFAT   "` marker; FAT32 identified by `FATSz16 == 0 && root-entry-count == 0`; `bps` not in 512/1024/2048/4096, `spc` not a power of two, out-of-range reserved sectors or FAT count are all refused
- UTF-16LE → UTF-8: BMP, surrogate pairs (U+1F600 must emit 4 bytes), control characters dropped, `0x0000` / `0xFFFF` treated as terminators, truncation instead of overflow when `cap` is short
- 8.3 short field: trailing spaces are padding and are stripped, embedded spaces are part of the name and are kept, `"NO NAME"` and all-spaces both count as "no label", non-ASCII is refused outright
- Root-directory entries: exFAT `0x83` (`0x03` deleted entries do not count, character count > 11 is malformed), FAT `0x0F` long-name entries reassembled by `order` (a deleted entry `0xE5` must clear a half-accumulated long name)
- End-to-end geometry self-consistency: the same synthetic sector fed to both layers must place the real root directory exactly at `data_off + (cluster - 2) × spc × bps`

> **Two pitfalls when generating fixtures**: ① `label_from_83()` by contract reads a full 11 bytes; feeding it an 8-byte literal reads past the end and segfaults on the host — always pass enough length when handing a literal to a fixed-size field under test. ② When building a boot sector, **the 8.3 label goes into one set of offsets only** (FAT12/16 at `0x2B`, FAT32 at `0x47`). Writing both overwrites FAT32's root-directory first cluster (`0x2C`-`0x2F`) along with the FSInfo, and the parsed cluster number becomes bytes of the name itself (writing `"OLDDISK"` reads back `0x4944404C` = `"LDDI"`).

This layer exists to exhaust locally the parts that have nothing to do with the PS5 yet are the easiest to get wrong. The earlier `%2F` defect was one of these — it only surfaced on the console, which is an expensive place to find it.

### HTTP end-to-end layer (259)

This suite compiles the **unmodified** `usbmanage.c` (not a character changed) into a host executable that listens on a real port, then drives it endpoint by endpoint with `curl`. PS5-specific calls are replaced by `hosttest/http/stub.c`:

| Replaced | Stub behaviour |
|---|---|
| `getfsstat` | Returns a controllable fake mount table (drive present / no drive / read-only diagnostics / with sandbox `nullfs` views / with synthetic labelled images) |
| `unmount` | Records calls; can simulate success, `EBUSY` failure, or "fails once then succeeds" |
| `sync` | No such call on Windows — empty implementation |
| `sysctl` | Fabricates the process / fd table from environment variables (`USBMANAGE_FAKE_PROCS` / `USBMANAGE_FAKE_FDS`) |
| `kill` | Records "whom, which signal" to `kill.log` and really removes the target from the fake process table (returns -1 when `USBMANAGE_FAKE_KILL_FAIL=1`) |
| `sceKernelSendNotificationRequest` | Writes the message to `notify_kernel.txt`, so we can assert that the second channel fired and what it carried |
| `getpeername` | Returns the source address from `USBMANAGE_FAKE_PEER_IP` (to test `/shutdown` loopback authentication) |

Three pure adaptations make this possible (**without touching `usbmanage.c`**): `prologue.h` (`-include`d; declares `sync` / `kill`, fixes the winsock `setsockopt` argument types, and redirects the `getpeername` macro to `usbmanage_host_getpeername` — the Windows import library already exports that name, so a same-named stub would raise `duplicate symbol`), `shim/` (for the BSD compatibility headers llvm-mingw dropped), and `-Dclose=usbmanage_host_close`.

> **How labels are genuinely tested on the host**: `hosttest/http/mkfixtures.py` generates five synthetic images (exFAT `0x83` with a Chinese name / FAT32 long-name entry plus short entry / FAT16 fixed root directory / FAT32 empty root falling back to the boot-sector label / pure garbage). The fake mount table uses **these files as device names**, so the real `fopen` / `fread` and cluster-offset arithmetic inside `read_volume_label()` all execute — they just read an ordinary file instead of a block device. They are deleted afterwards and never enter the ELF.

The fourteen scenarios:

| Scenario | Fake mount table | What it verifies |
|---|---|---|
| A two USB drives present | `/` + ext0 + ext1 + usb0 + usb1 | `/version` (both names), **`GET /` serves bytes identical to `web/index.html`**, **the browser and notification interfaces really are called at startup** (arguments, order, return codes, and pointing at the LAN IP rather than 127.0.0.1), **the notification JSON matches the official `rawData` structure with no self-invented actionUrl and a non-hard-coded timestamp**, `/diag` fields, `/list` showing only external volumes, both encoded and plaintext `/mnt/ext0` refused, trailing slash / path traversal / `/mnt/ext2` / empty argument refused, **browser navigation to `/list` or `/eject` 302-redirected to `/`**, `%2Fmnt%2Fusb0` actually ejecting, **`/icon.png` and `/favicon.ico` both returning `image/png` with bytes identical to `web/icon.png`**, **the page really declaring `<link rel="icon">` and the header logo**, unknown endpoints, POST 405 |
| B no USB drive | ext0 + ext1 only | `/list` returns `count:0`; internal volumes never enter the ejectable list |
| C volume busy | unmount returns EBUSY | `ok:false` + errno + a hint with the next action + `release_url`, holders inlined, `force=1` really passing `MNT_FORCE` down |
| D very many mount points | 80 fake sandbox entries with usb0/usb1 last | **the USB drives are not hidden by the cap**, `count` reports the true total of 82, `truncated:true`, still ejectable after truncation |
| E `--no-ui` | same as A | the browser interface is **not** called at startup, `ui_attempts:0`; a manual `/openui` still works |
| F system call refused | stub returns -1 | `/diag` reports the failure code honestly; the service, page and list are unaffected |
| G holder discovery and release | fake process table with killable and unkillable processes | `/holders` reports the holders (`init`, `SceShellCore`, and this service itself correctly excluded from "terminatable"), `/release` previews without acting, `confirm=1` really sends TERM, retry after release succeeds |
| H fd table refused | `KERN_PROC_FILEDESC` fails | reports "cannot tell" honestly instead of lying "nobody holds it"; `/release` skips termination yet still tries detaching mounts and forced unmount, and only reports `unmounted:false` if force fails too |
| I this service holds the volume | itself in the fd table | **never kills itself**: kills other holders only, puts itself in `skipped`, stays alive |
| J sandbox `nullfs` holds it | usb0 referenced by two sandbox views, no process fd | on failure `holder_count:0` + a "system sandbox" hint + `release_url`; `/release` **detaches both mounts**, `child_released_count:2`, **order correct (mounts before the target)**, and the final unmount succeeds |
| K labels | five synthetic images mounted at usb2…usb7 | each image type yields the right label; non-FAT and unreadable ones report an **empty string rather than an error**; internal volumes are never read; having a label does not affect ejection |
| L repeated load | same as A | **same version**: the new process exits by itself, the log says "activate", **the log contains no second `listening`**, the original instance's pid is not displaced; **different version**: takeover logs correctly, the new instance takes the port, the old instance really exits, `ui_reason:"takeover"` |
| M `/shutdown` authentication | same as A | non-loopback source without the marker header → refused, service still up; **opening `/shutdown` in the address bar is 302'd**; non-loopback with `X-Requested-With: usbmanage` → allowed; loopback without the header → allowed |
| N UI language | same as A | server-side decision from `Accept-Language`: no header / `en-US` / `ja-JP` → English; `zh-CN` / `zh-Hans-CN` → Simplified; `zh-TW` / `zh-HK` / `zh-Hant` → Traditional. No text from one language leaks into another (no Traditional characters in the Simplified document, and vice versa). `?lang=` overrides in all four directions. Screen notifications follow the process environment variable `LANG` |

> **Scenario L went red once, and it exposed a real bug**: the first implementation waited for `bind` to fail and then asked the old instance to retire, but on Windows the old instance never retired — because `SO_REUSEADDR` lets two processes listen on the same port simultaneously (the opposite of BSD). Moving the request *before* `bind` made host behaviour match the console.

### Page-JS layer (73)

The first two layers only exercise the API — **not one line of the page's JavaScript ever ran**, which is exactly how the "button wiped out by `load()`" bug escaped: the server faithfully returned `holders` / `hint` / `release_url` and every API test was green, but the page's `renderHolders()` appended the button to the card and immediately called `load()`, which rebuilds the whole table via `box.innerHTML = html` — the button and the holder list were wiped on the spot. On the console that looked like: a top banner telling you to "release the holders and eject", with no such button anywhere on the page.

This layer extracts the `<script>` source from `web/index.html` verbatim, feeds it a minimal DOM (`createElement` / `appendChild` / `innerHTML` / `querySelector` plus a synchronous `XMLHttpRequest` stub), then clicks buttons like a user and asserts that **the button really appears in the DOM**. Coverage:

- After a failure (EBUSY, or holders not discoverable) the card shows a "release holders and eject" button, on the right mount point, with the right wording, and the card is marked red
- When there are no holders it explains "usually a sandbox `nullfs` mount"
- The top banner reports the reason and names the drive; when it appears it gives `body` room (it no longer covers the title)
- Clicking release really issues `/release` with `confirm=1`; on success the volume disappears from the list
- Still failing after release: the retry button stays and the remaining holders are listed
- **Counter-proof**: calling `load()` right after `renderHolders()` is guaranteed to lose the button — proving these assertions really do catch that bug
- Label display: a labelled card shows a label row; an unlabelled one says "（这块盘没有卷标）" explicitly rather than leaving it blank; the confirmation dialog names the label
- Close service: `/shutdown` is really issued, carrying `X-Requested-With: usbmanage` and `Accept: application/json`; after that the list area becomes "service closed" and every button is disabled, and **further refreshes issue no requests at all** (the `dead` flag short-circuits them)

> **The version number is not hard-coded in the tests**: `run.sh` reads `#define VERSION` out of `usbmanage.c` and passes it to `page_test.js`. It used to be hard-coded, so the tests stayed green after a version bump — version drift must be caught by the tests, not by a human noticing.

---

## On-device acceptance

Host tests cannot prove how the system calls behave on the console. That has to be done on the console.

**Step 1: the UI and the startup notification.** After loading the payload, the TV should open a browser automatically and show the selection page, and a system notification should appear in the corner (carrying the management address `http://<LAN_IP>:9100`). The footer should read `v1.0.0`, the app icon should be visible at the top left, and the browser tab should use it too. If not:

- Notification missing but UI up → check `curl -s "http://<PS5_IP>:9100/diag"`, fields `rc.notify` and `rc.knotify`: both non-zero = both refused by the console; one zero and one not = one channel is not accepted on your machine; both zero but nothing on screen = the request was accepted and the console simply did not render it, which is console-side behaviour with no software-side fix
- `rc.browser` non-zero → the console refused to launch the browser; the code is the reason
- `ui_attempts: 0` → it never got that far (`--no-ui` in use, or the payload is not running)
- `procscan.supported` false → the kernel did not allow `KERN_PROC_FILEDESC`, holders cannot be discovered, and "release holders and eject" will automatically skip termination and fall back to mount detach plus forced unmount
- The page itself will not open (9100 unreachable) → the payload is not running
- Page opens but the version is wrong → an old instance is sitting on 9100. Loading a new payload in the toolbox does **not** evict the old one; click "close service" on the page first, then load the new one
- Auto-launch not working does not hurt usability either: `python usbmanage.py ps5-ui`, or open `http://<PS5_IP>:9100/` from a phone or PC, and you are in

**Step 1b: repeated load.** With it already running, click the toolbox's "load" again. Expect: ① the UI pops up again (that is the "activate" path); ② `/diag` still reports `ui_reason` = `start` (the instance on the port is the original one); ③ the console's process manager shows exactly one `payload.elf`. If `ui_reason` turns into `takeover`, you rebuilt the ELF and the version changed — that is by design, and `other_version` tells you which version was displaced.

**Step 1c: "close service".** Click it on the page and confirm. Expect the UI to become "service closed" with every button disabled; after that `curl` to 9100 should fail to connect. Load it again from the toolbox and it should come up normally (`ui_reason` back to `start`).

**Step 2: command-line verification.**

```bash
curl -s "http://<PS5_IP>:9100/version"     # must match the local ELF version, and carry both names
curl -s "http://<PS5_IP>:9100/diag"        # the four rc values; procscan.supported decides holder discovery
curl -s "http://<PS5_IP>:9100/list"        # external volumes only; check each label really is that drive's name
curl -s "http://<PS5_IP>:9100/list?all=1"  # internal volumes should be ejectable=false; look at count/shown
# icon: expect 200 / image/png / 12863
curl -s -o /tmp/icon.png -w '%{http_code} %{content_type} %{size_download}\n' "http://<PS5_IP>:9100/icon.png"
curl -s "http://<PS5_IP>:9100/eject?mount=%2Fmnt%2Fext0"   # must be refused (also exercises decode + allow-list)
```

> If the fourth one returns an empty `label`, do not jump to "bug" — the drive may simply have no label. Set one from Windows (drive properties → rename), reinsert and retest. The icon downloaded by the fifth can be compared directly against the repo's `web/icon.png` (`cmp`); identical bytes prove the ELF burns that exact image. The sixth does nothing even when refused, so it is safe to run.

**Step 3: real ejection and holder release.** Click "safe eject" on a **USB drive** in the UI (do not click internal storage; those are disabled in the UI anyway):

- Immediate success → "unmounted" confirmation; only now is it safe to unplug
- Failure (EBUSY) → the clicked card should show a "who is holding it" area and a "release holders and eject" button, **and the button must appear whether or not holder processes were found**, with the card marked red. Clicking it gives one of three clear outcomes: success / partial (terminated N processes and detached M mounts but still failed) / failure (reason plus remaining holders)
- If it still will not unmount with "no program using it", send the outputs of `/holders` and `/list?all=1` — the latter shows whether sandbox `nullfs` mounts are referencing the drive
- On success, note two fields in the receipt: `child_released_count` (how many sandbox mounts were detached; non-zero confirms the "no process yet EBUSY" case is `nullfs`) and `forced_retry` (whether `MNT_FORCE` was applied)

**Step 4: cross-check against the official sample.** Compile the SDK's own `samples/mntinfo` (the official mount-enumeration sample, closest to this payload's needs) with exactly the same flags, then compare structures side by side:

```bash
SDK=.../ps5-payload-sdk
clang ... "$SDK/samples/mntinfo/main.c" -o mntinfo-ref.elf   # same flags as build.sh
llvm-readelf -h/-l/-d usbmanage.elf mntinfo-ref.elf
```

Measured, they agree item by item:

| Item | usbmanage.elf | mntinfo-ref.elf |
|---|---|---|
| Class / ABI / Type / Machine | ELF64 / FreeBSD / DYN / x86-64 | same |
| Entry point | `0x0` | `0x0` |
| NEEDED | libkernel_sys, libSceLibcInternal, libSceNet, libkernel_web | same (same order) |
| Program headers | 3×LOAD (RWE/RW/RW, align 0x4000) + DYNAMIC | same |

In other words, on every dimension the loader can see, this artifact matches the official sample; `unmount` / `getfsstat` / `socket` / `sync` are all in the dynamic symbol table and resolved at runtime by the console's four libraries.

---

## Deploying to the console

The console runs **pldmgr or DB's jailbreak toolbox** (`<PS5_IP>:7788`, built on pldmgr). It already provides HTTP interfaces, so there is no need for 9021, for elfldr, or for etaHEN.

### Where the toolbox "discovers" an ELF

pldmgr does **not** scan the whole disk; it only looks at a few fixed places. In order of reliability:

| Location | Notes | Evidence |
|---|---|---|
| `/data/pldmgr/payloads/<name>/<file>.elf` | Internal storage. The toolbox's own data directory, where its upload interface lands | **Measured on our console**: `usbmanage.elf` landed at `/data/pldmgr/payloads/usbmanage/usbmanage.elf` |
| A `pldmgr/` subdirectory under `/mnt/usb0`…`/mnt/usb7` | Create a `pldmgr` folder in the drive root, drop the ELF in, plug it in and it is discovered — **no copying to internal storage needed** | PLK docs + community guides (not reproduced on our console) |
| `/data/ps5_autoloader/` | The Y2JB autoloader directory. Pair with `autoload.txt` for boot-time loading | PLK autoloader docs |
| `/data/etaHEN/payloads/` | etaHEN's directory. We do not run etaHEN so it is unused — listed only to avoid confusion | official guides |

The subdirectory is named after the payload (`usbmanage.elf` → `usbmanage/`). **File names are case-sensitive.**

**"Discovery" and "loading" are two separate actions**: dropping the file in any location above makes it discoverable (it appears in the list), but actually running it means `GET http://<PS5_IP>:7788/loadpayload:<full path>` — the "load" button on the plugin card in the UI. Once loaded, every payload's process is called `payload.elf`, so do not identify yourself by process name; use `/version` on port 9100.

### What is that `.json` in the payload directory?

```
/data/pldmgr/payloads/
├── usbmanage/
│   ├── usbmanage.elf            ← the payload binary
│   └── usbmanage.elf.json       ← metadata generated by the toolbox
└── …
```

**That `<filename>.json` is written by the toolbox itself and needs no manual preparation** (same timestamp as the ELF). Its `install_source` distinguishes three origins: `web_upload` (uploaded through the web UI), `usb` (imported from a USB drive, with `install_source_detail` recording the path), `repository` (downloaded from the cloud repository, with url / version / checksum / category).

It **plays no part in discovery or loading**: the `/list_payloads` array comes from scanning the directory and `meta` is only display decoration. The file in the right directory is enough; losing the json costs you a few fields.

Optional cosmetic tweak (not required; back up before editing): for an uploaded payload the json's `name` defaults to the filename, which is what the UI shows. To get a clean display name and a description, edit:

```json
{
  "name": "PS5 USB管理器",
  "filename": "usbmanage.elf",
  "description": "PS5 USB管理器（usbmanage）：枚举 /mnt/usb* 并 sync + unmount，HTTP 9100",
  "version": "1.0.0",
  "downloaded_at": "…leave the original value…",
  "install_source": "web_upload"
}
```

> Leave `checksum` empty — if some auto-update flow starts using it for verification, a wrong value is worse than none.

### Getting the ELF in (four ways, pick one)

| # | Method | When it fits | Where it lands |
|---|---|---|---|
| 1 | **Straight on a USB drive**: create `pldmgr/` in the drive root, drop the ELF in, plug it into the PS5 | Touches neither PC nor network — the least work | `/mnt/usb<N>/pldmgr/`, no copying |
| 2 | **Toolbox web UI**: `http://<PS5_IP>:7788` → plugin / management page, pick the file in a browser | You have a PC or the console's own browser | The toolbox decides (measured: `/data/pldmgr/payloads/usbmanage/`) |
| 3 | **Toolbox USB import**: the "USB files" entry in the UI, or `GET /usb_move_check?path=` + `GET /usb_move_perform?path=` | USB drive as an intermediary, but it must land in internal storage | `/data/pldmgr/payloads/` |
| 4 | **FTP / web file manager**: ftpsrv (2121) or web-file-mgr (8888) | You want to choose the path yourself | Put it at `/data/pldmgr/payloads/usbmanage/usbmanage.elf` by hand |

> Method 1 requires the drive to be in a filesystem the PS5 accepts (exFAT / FAT32). Note that **this tool's whole job is to eject USB drives** — do not eject the one holding the payload while testing.

### The web file manager (8888) — another upload and launch channel

`ps5-web-file-manager` runs on 8888, a web file manager with upload / download / edit / launch built in. Measured backend endpoints (**paths never carry a leading `/`**):

| Endpoint | Purpose |
|---|---|
| `GET /api/list?path=` | List a directory |
| `GET /api/text?path=` | Read a text file |
| `GET /fs?path=` | Fetch a file's raw content |
| `POST /api/download/prepare` → `GET /api/download?id=` | Download |
| `POST /api/upload/prepare` → `POST /api/upload-file` → `POST /api/upload/finish` | Upload |
| `POST /api/launch-elf` (`path=`) | **Launch an ELF directly** |

So **you can inject and launch without the toolbox**: upload the ELF anywhere, then `launch-elf`. The cost is that it does not write pldmgr's metadata json, so "visible in the toolbox list" still requires the toolbox upload path. That payload is not resident: once closed or crashed, nothing listens on 8888 until it is loaded again.

### Console-side verification (the loop)

```bash
curl -s "http://<PS5_IP>:7788/list_payloads"   # discovery means usbmanage.elf shows up here

# 1) upload (body = raw ELF bytes)
curl -X POST --data-binary @usbmanage.elf \
  "http://<PS5_IP>:7788/manage:upload?filename=usbmanage.elf"

# 2) confirm where it landed
curl -s "http://<PS5_IP>:7788/list_payloads"

# 3) launch (use the actual path returned above)
curl -s "http://<PS5_IP>:7788/loadpayload:/data/pldmgr/payloads/usbmanage/usbmanage.elf"
```

The one-command equivalent: `python usbmanage.py --ip 192.168.1.100 ps5-deploy`.

Self-check after launch:

```bash
curl -s "http://<PS5_IP>:9100/version"        # names + version + language
curl -s "http://<PS5_IP>:9100/diag"           # UI auto-launch state + syscall return codes + procscan
curl -s "http://<PS5_IP>:9100/list"           # ejectable external volumes (count:0 when no drive)
curl -s "http://<PS5_IP>:9100/list?all=1"     # all mount points, internal and sandbox included (read-only)
curl -s "http://<PS5_IP>:9100/holders?mount=%2Fmnt%2Fusb0"              # who holds usb0
curl -s "http://<PS5_IP>:9100/release?mount=%2Fmnt%2Fusb0"              # preview the release (does not act)
curl -s "http://<PS5_IP>:9100/release?mount=%2Fmnt%2Fusb0&confirm=1"    # really release and eject
curl -s "http://<PS5_IP>:9100/eject?mount=/mnt/usb0"                    # literal form
```

> If `/list` returns `count:0` and `/list?all=1` shows only internal volumes after `ps5-deploy`, no USB drive is plugged in — that is the normal state; the allow-list will not list internal volumes.

### Autoload at boot (optional)

The jailbreak is tethered, so after a reboot every payload is gone. Besides running `ps5-deploy` every time, you can set this one to autoload from the toolbox's own UI:

1. Open `http://<PS5_IP>:7788` → the "autoload" page
2. Find `usbmanage.elf` under "available plugins" and add it to the list (that page converts wait markers such as `DELAY` into payload syntax; use it rather than hand-editing the config)
3. Turn the autoload master switch on

> The configuration is read via `/get_config` and written via `/set_config` (the same `pldmgr_config.txt`), fields `AUTOLOAD_ENABLED` and `AUTOLOAD_LIST`. **This project deliberately does not write those two fields**: the `!` prefix and similar syntax inside `AUTOLOAD_LIST` is generated by the toolbox front end, and a hand-written mistake can break the boot chain — the downside is not symmetric.
>
> The toolbox also recommends "no more than 4 autoload entries"; check how many already run on your console before adding one.

---

## API

The payload serves HTTP on the console at port **9100**.

| Method | Path | Purpose |
|---|---|---|
| GET | `/`, `/index.html` | Built-in web UI (page and icon are baked into the ELF) |
| GET | `/ping` | Liveness probe, returns `pong` |
| GET | `/version` | Names (Simplified / Traditional / English), version, current language (`zh-Hans` / `zh-Hant` / `en`). **On repeated load the new instance identifies the incumbents as its own version through this endpoint** |
| GET | `/list` | Enumerate ejectable USB volumes: mount point, capacity, label |
| GET | `/list?all=1` | All mount points (read-only diagnostics, `ejectable` marks which can be detached) |
| GET | `/eject?mount=/mnt/usb0` | Eject. On failure returns the holders and a `release_url` inline; add `force=1` for `MNT_FORCE` |
| GET | `/holders?mount=/mnt/usb0` | Who holds this volume: pid / process name / open paths / whether it can be safely terminated |
| GET | `/release?mount=/mnt/usb0&confirm=1` | Release holders and retry the unmount (TERM→KILL → detach `nullfs` mounts → sync + unmount). **Requires `confirm=1` to act** |
| GET | `/procs` | Process list (read-only diagnostics: pid / ppid / uid / name / whether it is this service) |
| GET | `/openui` | Make the console throw the UI onto its own screen; can be triggered remotely from a PC or phone |
| GET | `/diag` | Diagnostics: version, port, LAN address, UI auto-launch state, syscall return codes, whether process scanning is available |
| GET | `/icon.png` | Application icon (`/favicon.ico` returns the same bytes) |
| GET | `/shutdown` | Exit the process entirely (used by the page's "close service" button) |

Startup arguments: `usbmanage.elf [port] [--no-ui] [--force]`. `--no-ui` disables "launch the UI after loading" and keeps only manual `/openui`. `--force` skips the "same-version instance already present" decision and starts a new process that takes the port — **for host testing only**; on the console you get two instances fighting over one port.

Opening `/list` or `/eject` directly in a browser 302-redirects to `/` (trigger: the `Accept` header contains `text/html`). The page's own XHRs send `Accept: application/json` explicitly. Use `curl` when you want JSON. The flip side of this behaviour is a feature: `/eject` cannot be triggered by an accidental press of Enter in the address bar, only from the UI.

Example `/diag` response:

```json
{"name":"PS5 USB管理器","name_hant":"PS5 USB管理器","name_en":"PS5 USB Manager",
 "version":"1.0.0","lang":"en","port":9100,
 "lan_ip":"192.168.1.100","ui_autolaunch":true,"ui_attempts":1,
 "ui_reason":"start","other_version":"",
 "rc":{"user_init":0,"browser":0,"notify":0,"knotify":0},
 "procscan":{"supported":true,"procs":42}}
```

In `rc`, `-999` means "not attempted"; any other non-zero value is a failure code from the console. `rc.notify` is the return code of `sceNotificationSend` and `rc.knotify` of `sceKernelSendNotificationRequest` — **which channel the console declines to display is exactly what these two numbers tell you**. `procscan` reports whether process scanning (`sysctl KERN_PROC_FILEDESC`) is usable.

`ui_reason` explains why the UI was launched this time:

| Value | Meaning |
|---|---|
| `start` | Normal startup |
| `same-version` | Repeated load: a same-version instance is already running, this process only brings the UI forward and exits |
| `busy` | Something on the port connected but says nothing; treated as "an instance", so it only activates |
| `takeover` | The port was held by a **different version**; this process retires it and takes over. `other_version` is the displaced version |

Example `/list` response (measured on the console):

```json
{"count":2,"scope":"usb","volumes":[
  {"mount":"/mnt/usb0","device":"/dev/da2p1","fstype":"exfatfs","ejectable":true,
   "label":"移动硬盘",
   "bsize":131072,"blocks":16776673,"bfree":1022447,
   "total":2198952083456,"free":134014173184},
  {"mount":"/mnt/usb1","device":"/dev/da2p2","fstype":"exfatfs","ejectable":true,
   "label":"",
   "bsize":131072,"blocks":21376583,"bfree":8941511,
   "total":2801871486976,"free":1171981729792}
],"shown":2,"truncated":false}
```

`count` is the **true total number of matches**, `shown` is how many were actually listed this time, and `truncated` marks whether the two differ. They are reported separately because the console has a great many mount points (80+ measured) and the diagnostic view `/list?all=1` gets cut off by `MAX_VOLUMES` (64) or by the response buffer; reporting "entries written" as the total would create the illusion of "the console clearly has a drive but the API says count is only in the dozens". `/list` (external volumes only) has very few entries and is not affected by truncation — test scenario D guards exactly this: 82 mount points with the USB drives last must still be listed and ejectable.

`/eject` success and failure (volume busy):

```json
{"ok":true,"mount":"/mnt/usb0","forced":false,"msg":"已落盘并卸载，现在可以安全拔出了"}
```

```json
{"ok":false,"code":16,"mount":"/mnt/usb0","forced":false,"busy":true,
 "msg":"卸载失败：Resource device。谁在占用见列表，点「解除占用并卸载」自动处理",
 "holders_supported":true,
 "holders":[{"pid":99,"comm":"web-file-mgr.elf","fd":12,"kind":"fd",
             "path":"/mnt/usb0/movie.mkv","self":false,"killable":true},
            {"pid":10,"comm":"SceShellCore.elf","fd":5,"kind":"fd",
             "path":"/mnt/usb0/b","self":false,"killable":false,
             "why":"Sony 系统组件（Sce* 前缀，不代为终止）"}],
 "holder_count":2,
 "release_url":"/release?mount=/mnt/usb0&confirm=1"}
```

When holder processes cannot be found, `msg` and `hint` explain why (fd table refused / holders come from sandbox mounts), and **"release holders and eject" is available either way** — `/release` skips termination, detaches the system mounts and force-unmounts. `holder_count:0` does not mean "try again".

A successful `/release`:

```json
{"ok":true,"mount":"/mnt/usb0","confirmed":true,"before":2,"scanned":42,
 "holders":[…],"killed":[{"pid":99,"comm":"web-file-mgr.elf","sig":"TERM"}],
 "skipped":1,"after":0,"still":[],
 "child_released":[{"mount":"/mnt/sandbox/CUSA12345_000/mnt/usb0","ok":true,"err":0}],
 "child_released_count":1,"unmounted":true,
 "msg":"已解除占用并卸载（已终止占用进程）（已解除系统内部挂接），现在可以安全拔出了"}
```

`child_released` is the receipt for each `nullfs` mount whose source pointed at this drive being detached one by one. The application sandbox mounts the USB drive into references such as `/mnt/sandbox/<TITLEID>_000/...` that belong to no process (the fd table shows nobody), yet unmounting the target volume returns EBUSY — on the console, "nothing is using it but it will not unmount" is usually this. When the final ordinary `unmount` fails, `MNT_FORCE` is applied automatically (the receipt carries `"forced_retry":true`), and the outcome of every step is reported honestly.

> `%2F` in the query string is decoded before hitting the allow-list. Python's `urlencode` encodes `/` as `%2F` by default; the console used to compare literally, refusing legitimate requests as illegal paths — which looked like "I clicked eject and nothing happened". The console now decodes first, and the PC side keeps slashes with `safe="/"`, so the two layers no longer depend on each other.

---

## Single instance (repeated load and takeover)

Loading a payload in the toolbox has two established behaviours that directly affect upgrades: **every payload's process is called `payload.elf`** (so process names cannot tell you which one is ours), and **loading a new payload does not kill the previous instance** (the UI even says "do not load a plugin twice"). Upgrades therefore run into: new instance starts → `bind(9100)` fails → the old instance keeps serving → the deploy "looks successful" while the version never actually changed.

The current decision logic (`probe_instance()`, 600 ms timeout):

| State of the port | Action |
|---|---|
| Nobody listening | Normal startup |
| `/version` self-identifies as usbmanage **and the version matches** | **Only brings the UI to the screen, then this process exits** (process count stays 1) |
| `/version` self-identifies as usbmanage but the **version differs** | Sends `/shutdown` to retire it, waits 500 ms, takes the port |
| Connected but says nothing | Treated as "an instance"; likewise activates only, does not start a second |
| Something else is answering | Errors out, **does not steal another program's port** |

Two things must be probed rather than inferred from process names:

- Every payload in the toolbox is called `payload.elf`, so "look for my own name in the process list" is a dead end; only the port plus a `/version` self-description works.
- **Takeover must happen before `bind`, not as a repair after it fails**: on Windows `SO_REUSEADDR` lets two processes listen on the same port simultaneously (the opposite of BSD), so by the time `bind` errors the old instance is still alive. Requesting retirement up front behaves identically on FreeBSD and Windows.

> If the old instance is an earlier version without `/shutdown`, takeover fails and prints a hint; retire it by hand — this can only happen on an upgrade path.
>
> One more note: **deleting the ELF file on the console does not kill the running process**. As long as the old instance lives, it keeps holding 9100.

---

## Volume labels

An empty `label` means the drive **has no label** (or it could not be read) — it is not an API error. This is only a helper so people do not eject the wrong drive, and it **never affects whether a volume can be ejected**.

The `/mnt/usb0` → `/dev/da2p1` mapping carries no readable meaning on its own, and the PS5 has no eject entry point, so ejecting the wrong drive costs you a corrupted filesystem. The label is the only identifier recognisable at a glance, so the boot sector has to be read directly — `struct statfs` has `f_mntonname` / `f_mntfromname` / `f_fstypename` and **no label field**.

| Filesystem | Where the label lives |
|---|---|
| exFAT | Boot sector `"EXFAT   "` marker → cluster-heap offset (`0x58`) + root-directory cluster (`0x60`); find the `EntryType 0x83` entry in the root directory (`bit7` = in use, second byte = character count), the name is UTF-16LE |
| FAT32 | `FATSz16` (`0x16`) = 0 and root-entry-count (`0x11`) = 0 → data area = `(reserved sectors + FAT count × FAT size + root-dir sectors) × bytes-per-sector`, root-dir first cluster at `0x2C`; reassemble the full name from `0x0F` long-name entries first, then fall back to the 8.3 `0x47` field |
| FAT12/16 | The root directory is a fixed area, the 8.3 label sits at boot-sector `0x2B` |

The PS5 only accepts exFAT and FAT12/16/32 for external storage, so only those three are implemented. Internal volumes (`/system`, `/user`, `/mnt/sandbox/**`) are never read at all — they are neither external drives nor labelled, and reading them would only add risk for nothing; `label` is filled in for `/mnt/usb<number>` only.

Two details: **the 8.3 short field accepts pure ASCII only**; a Chinese label is GBK bytes in that field and printing them raw yields mojibake, so non-ASCII is abandoned in favour of hunting the long-name entry (`0x0F`) in the root directory; `"NO NAME"` is Windows' all-spaces placeholder and is likewise treated as absent. Reading uses `fopen` / `fread` / `fclose` rather than `open` / `read` — under a host build `close()` is swapped for the socket version, and CRT file descriptors and socket handles live in different domains.

---

## App icon

The browser tab no longer shows a default question mark, and the header uses the same image.

| Endpoint | Returns |
|---|---|
| `GET /icon.png` | 96×96 PNG, 12863 bytes, `Content-Type: image/png` |
| `GET /favicon.ico` | **The same bytes.** Browsers sniff by content, so a PNG served from an `.ico` path is fine; that path is also the fallback for old browsers that ignore `<link rel="icon">` |

Like the page, the icon is compiled into the ELF (no external files, no network): `web/icon.png` becomes `web/icon_png.h` via `xxd` at build time. The page references it twice:

```html
<link rel="icon" type="image/png" href="/icon.png">        <!-- head -->
<img class="logo" src="/icon.png" width="40" height="40">  <!-- header -->
```

The master file, the size ladder and the spec (RGBA, transparent corners, ~4% transparent margin, ~20% corner radius) are documented in [`assets/icon/README.md`](../assets/icon/README.md). The same 96×96 file serves both the PS5 page and the PC console; at the 32–40 px display size it is a 2–3× asset. The header logo links `/icon.png` directly — do not inline base64, which would inflate the page bytes by an order of magnitude.

> **Changing the icon requires re-running `bash build.sh`**: the icon is burned into the ELF, so without a rebuild the ELF still carries the old one. `build.sh` self-checks two things at the end — that the generated array length matches `web/icon.png`, and that the PNG `IHDR` marker is present inside the ELF (to catch "the array is never referenced so the linker dropped it", which a length check alone cannot see).

---

## Safety model

There is one rule, and it is positive: accept `/mnt/usb` plus digits, refuse everything else.

Names are not used as a heuristic because the console's internal mount names change with host state. Measured on 2026-10-07, `/list?all=1` on a real console (80+ mount points) looked like this:

| Class | Mount point / device | Nature |
|---|---|---|
| External USB | `/mnt/usb0` = `/dev/da2p1`, `/mnt/usb1` = `/dev/da2p2` (exfatfs, 2.2 TB / 2.8 TB) | **only this prefix can be unmounted** |
| Internal | `/` = `md0` (exfatfs), `/system` = `/dev/ssd0.system`, `/system_ex`, `/system_data` (ufs), `/user` (bfs), `/update`, `/preinst` | the console's internal storage |
| Internal (mounts) | `/data` = nullfs→`/user/data`, `/original_user`, `/original_system_data`, `/devlog/*` | aliases / mounts, not independent volumes |
| System | `/dev` (devfs), `/system_tmp` (tmpfs), `/mnt` (tmpfs) | pseudo filesystems |
| Sandbox | `/mnt/sandbox/<TITLEID>_000/...` (a large batch of nullfs) | sandbox views of VSH applications |

An earlier session instead enumerated `/mnt/ext0` (ufs) and `/mnt/ext1` (bfs) — **there is not a single `/mnt/ext*` on today's machine**. In other words internal mount names vary with host state and counting on names will fail eventually: `/mnt` itself is tmpfs and `/mnt/sandbox/**` are sandbox `nullfs` views, so a coarse "starts with `/mnt`" test would let them all through. The mount point's existence is verified once more immediately before unmounting.

> A pitfall we hit: the first allow-list included `/mnt/ext*`, and with no USB drive attached the enumeration returned exactly the internal volumes — so a "detach everything" would have unmounted the console's internal storage. It is now tightened to `/mnt/usb<number>`, and the full view moved to the read-only `/list?all=1`.

A few more:

- Every eject goes `sync` → `unmount` → read-back verify; none of the three is optional. exFAT only lowers the cost of a mistake; it is not a substitute for unmounting
- Release only touches `fstypename == "nullfs"` mounts whose source is the target mount point, nothing else
- Release does terminate other processes (TERM first to give them a chance to finish, KILL if still holding afterwards). Guard rails stand in three places and are never touched: **this service itself, pid ≤ 1 (kernel/init), and `Sce*` Sony system components**. Better to leave one holder the user must handle themselves than to kill something critical by mistake
- The PS5-side API is unauthenticated (except `/shutdown`). Use it on a controlled LAN only, and **never expose it to the internet**
- `/shutdown` accepts only two kinds of request: loopback origin (how a new instance takes over), or a page request carrying `X-Requested-With: usbmanage`. That blocks CSRF, and an accidental paste into the address bar along with it

---

## Naming

| | Name | Used for |
|---|---|---|
| Chinese display name | **PS5 USB管理器** | Simplified-Chinese UI titles, on-screen notifications, the `name` field of `/version` — anywhere a human looks |
| Traditional display name | **PS5 USB管理器** | Traditional-Chinese UI and notifications, the `name_hant` field of `/version`. Currently identical to Simplified (those three characters are the same in both scripts); the separate field exists so that a future divergence only has to be changed in one place |
| English display name | **PS5 USB Manager** | English UI and notifications, the `name_en` field of `/version` — anywhere a human looks |
| Machine identifier | **usbmanage** | Payload filename `usbmanage.elf`, the plugin directory name in the toolbox, the JSON `app` field, the CSRF header `X-Requested-With: usbmanage` — anywhere a machine looks |

**Do not change the display name and the machine identifier together.** Changing the display name (UI text) is safe; changing the `usbmanage` identifier breaks the toolbox scan path, the deploy scripts, the CSRF header and existing documentation — pure risk, zero gain. There is also a subtler consequence: `probe_instance()` identifies its own kind through the machine identifier in `/version`, so changing the identifier makes **a repeated load treat the old instance as "someone else's program" and refuse to start** (we hit this while renaming).

---

## Design notes (selected)

- Display names and the machine identifier are separate. The UI and notifications use `PS5 USB管理器` / `PS5 USB Manager`; the payload filename, the JSON `app` field and the CSRF header stay `usbmanage`. Renaming a display name should not drag the identifier along, because the toolbox scan path and the deploy scripts both key on that string.
- Single instance is decided by "port + `/version` self-description", not by process name: inside the toolbox every payload's process is called `payload.elf`. Takeover happens before `bind`, because on Windows `SO_REUSEADDR` lets two processes listen on the same port simultaneously (the opposite of BSD) — by the time `bind` fails, the old instance is still alive.
- UI language is decided server-side, from `Accept-Language`, with a second copy of the logic in the page as a fallback. The three word tables live in the `T()` calls in `usbmanage.c` and the `L` table in `web/index.html` — **both must be edited together**, or you get "the heading changed but the body did not". On-screen notifications are emitted before any HTTP request exists, so they can only fall back to the `LC_ALL` / `LC_MESSAGES` / `LANG` environment variables.
- Volume labels are read straight from the boot sector. Two gates: the `55 AA` signature and a leading jump instruction. FAT32 is identified by `FATSz16 == 0 && root-entry-count == 0`. If the 8.3 field contains non-ASCII (Chinese is GBK there) it is discarded in favour of the long-name entry.
- `sha256` is not reproducible. The linker writes a random `build-id` on every link, so treat it as a transport-integrity fingerprint, not as a reproducible-build credential.
- Auto-launching the UI goes through the official Orbis interfaces, not through any toolbox mechanism — the toolbox has no such thing as "popping up a plugin window". The SDK ships `samples/browser` and `samples/notify`, and community payloads that "pop something up after loading" all take this route: the console launches its built-in browser at this service, then raises a screen notification.
- The UI is burned into the ELF. The page source is `web/index.html`, converted by `xxd` into a C byte array (`web/index_html.h`) at build time; the icon (`web/icon.png` → `web/icon_png.h`) uses the same mechanism. Changing the page means editing `web/index.html` and running `bash build.sh` — no external files, no network.
- The page's JavaScript is ES5 only (XHR; no Promises, arrow functions or template strings), so it opens in old environments such as the console's built-in browser.
- The page's XHRs must declare two headers explicitly: `Accept: application/json` — otherwise the request is judged a browser navigation and 302'd to the home page; and `X-Requested-With: usbmanage` — one of the credentials `/shutdown` accepts, needed because clicking "close service" from a PC browser is not a loopback origin and would otherwise be refused.
- In-place feedback has to go through `pending`: `load()` rebuilds the whole list via `innerHTML`, so any node appended directly to a card disappears on the next refresh. Eject and release results are therefore written into `pending` first, and after every `load()` repaint `applyPending()` re-attaches them to the matching card. When editing the page logic, do **not** call `load()` straight after `renderHolders()`.

---

## Repository layout

```
usbmanage/
├─ LICENSE                        GPL-3.0 full text
├─ README.md / README.en.md       User documentation (Chinese / English)
├─ CHANGELOG.md                   Release notes
├─ THIRD-PARTY-NOTICES.md         Third-party components and licence evidence
├─ config.example.json            Config template (config.json holds your LAN IP and is git-ignored)
├─ usbmanage.py                   PC-side CLI entry point
├─ server.py                      Local web console backend (stdlib only)
├─ web/                           Web console page
├─ assets/icon/                   Final application icon for all platforms
├─ usbmanage/backends/
│  ├─ ps5.py                      PS5 backend (upload via 7788, control via 9100)
│  └─ local_win.py                Windows removable drives
├─ device/ps5-usbmanage/          ★ the PS5 payload
│  ├─ usbmanage.c                 Source
│  ├─ web/index.html              Embedded UI (converted by xxd at build time)
│  ├─ web/icon.png                Embedded icon (likewise)
│  ├─ build.sh                    Build script (Git Bash / Linux / macOS)
│  ├─ hosttest/                   Three host-side test layers
│  └─ dist/                       Artifacts: usbmanage-v<version>.elf + SHA256SUMS.txt
└─ docs/
   ├─ DEVELOPMENT.md              Developer docs (Chinese)
   ├─ DEVELOPMENT.en.md           Developer docs, English (this file)
   ├─ images/                     UI screenshots used by the docs
   ├─ 根因分析与方案设计.md         Root-cause analysis and design (source, Chinese)
   └─ analysis.html               Same, HTML version
```

The images in `docs/images/` are not mock-ups: they are real browser renders of `device/ps5-usbmanage/web/index.html`, with a mock `/list` and `/eject` response matching the examples in these docs. **No line of the page was changed for them.** Re-shoot them when the page changes.

---
