# SIRIL on HarmonyOS PC (MateBook)

This tree builds SIRIL as a **native HarmonyOS application**: the SIRIL engine
(the same source that powers `siril-cli`) is cross-compiled for
`arm64-v8a`/OpenHarmony, packed into `libsiril-ohos.so`, and driven from an
ArkTS console UI.  Every SIRIL command is reachable in three ways:

1. the in-app command line (ArkTS UI → NAPI → engine),
2. a **TCP control port** speaking one JSON object per line,
3. the host CLI `scripts/siril-cli.mjs`, which also performs the `hdc` port
   forward and can list/describe/batch commands.

```
┌────────────────────────── MateBook (HarmonyOS) ──────────────────────────┐
│  ArkTS console (pages/Index.ets)                                         │
│      │ NAPI (libsirilohos.so)                                            │
│      ▼                                                                   │
│  libsiril-ohos.so  ── SIRIL core (libsiril.a) + bridge + TCP server      │
│      ▲                     │                                             │
│      │ 127.0.0.1:4780      └── processcommand() → 218 SIRIL commands     │
└──────┼───────────────────────────────────────────────────────────────────┘
       │ hdc fport tcp:4780 tcp:4780
┌──────┴─────────────── macOS host ────────────────────────────────────────┐
│  node scripts/siril-cli.mjs --command "stat"                             │
└──────────────────────────────────────────────────────────────────────────┘
```

## Status

- Engine: SIRIL 1.5.0-dev (`e17d9ffcf`), headless build (`-Dgtk=false`)
- Command catalog: **218 commands** generated from `src/core/command_list.h`
- Control port: JSON line protocol, loopback-only by default, optional token
- UI: console, quick actions, live status, port start/stop, log copy
- Verified on HUAWEI MateBook (HAD-W32, HarmonyOS 7.0.0, API 26)

## Build

```bash
# 1. cross-compiled dependency sysroot (see BUILD.md)
# 2. engine + HAP
scripts/build-ohos-app.sh native
scripts/build-ohos-app.sh hap

# 3. sign with DevEco/AGC debug material, then install
hdc install -r entry-default-unsigned.hap   # after signing, see BUILD.md
hdc shell aa start -a EntryAbility -b com.joinother.siril
```

Environment: `SIRIL_OHOS_BUILD` (build root), `SIRIL_OHOS_PREFIX`
(sysroot), `OHOS_NATIVE_SDK` (DevEco native SDK).

## Install and test

```bash
# device must be visible to hdc
hdc list targets

# forward the control port and talk to the engine
hdc fport tcp:4780 tcp:4780
node scripts/siril-cli.mjs --status
node scripts/siril-cli.mjs --command "help"
node scripts/siril-cli.mjs --command "stat"
node scripts/siril-cli.mjs --list
```

The CLI can also do the forwarding itself:

```bash
node scripts/siril-cli.mjs --device 192.168.1.56:43975 --command "stat"
```

## Test results (2026-10-28, HAD-W32)

See [TEST-REPORT.md](TEST-REPORT.md) for the command-by-command evidence,
screenshots and the FITS pipeline smoke test, and [BUILD.md](BUILD.md) for the
cross-compiled dependency stack and signing steps.

![SIRIL on the MateBook](images/siril-matebook.jpg)
