# SIRIL HarmonyOS CLI and control port

## Two layers

* **Engine commands** — the 218 commands SIRIL itself understands (`load`,
  `calibrate`, `register`, `stack`, `autostretch`, `seqplatesolve`, ...).
  They are listed by `--list` and described by `--describe`.
* **Control operations** — the small transport layer handled by the port:
  `ping`, `status`, `version`, `catalog`, `exec`, `chdir`.

## Protocol

One JSON object per line, newline terminated, on `127.0.0.1:4780`.

```json
{"id":"1","op":"exec","cmd":"load m31.fit"}
{"id":"1","ok":true,"code":0,"output":"...","ms":12,"status":{...}}
```

A line that does not start with `{` is treated as a raw SIRIL command, which
makes `nc 127.0.0.1 4780` usable for quick checks:

```bash
printf 'stat\n' | nc 127.0.0.1 4780
```

If the application was started with a token, add `"token":"..."` to every
request; requests without it are rejected with `invalid token`.

## Host CLI

```bash
node scripts/siril-cli.mjs --list                     # all commands
node scripts/siril-cli.mjs --describe stack           # usage + preconditions
node scripts/siril-cli.mjs --command "help"
node scripts/siril-cli.mjs --command "cd /data/storage/el2/base/files/work"
node scripts/siril-cli.mjs --command "load light_001.fit"
node scripts/siril-cli.mjs --command "autostretch -linked"
node scripts/siril-cli.mjs --command "save result"
node scripts/siril-cli.mjs --batch commands.json
node scripts/siril-cli.mjs --script pipeline.ssf
node scripts/siril-cli.mjs --interactive
node scripts/siril-cli.mjs --status --json
```

`--device <serial>` runs `hdc fport tcp:4780 tcp:4780` first, so a single
command works without touching the device shell.

Exit code is `1` when the engine reports a failed command, so the CLI can be
used in scripts and CI.

## Command catalog

`commands.json` is generated from SIRIL's own command table:

```bash
python3 scripts/gen-command-catalog.py -o harmonyos/entry/src/main/resources/rawfile/siril-data/commands.json
```

Each entry has `name`, `min_args`, `usage`, `scriptable` and `requires`
(`SINGLE_IMAGE`, `SEQUENCE`, `FOR_RGB`, ...).  The app copies the catalog into
its sandbox and serves it through the `catalog` operation.

## Security

* The listener is created on `127.0.0.1`; it never binds a public interface
  unless the caller asks for one explicitly.
* The port is reachable from the host only through `hdc fport`.
* Set a token when the port must be shared: the app accepts one through
  `startServer(bind, port, token)`; requests must then carry `"token"`.
* Long-running commands are serialised: one engine, one command at a time.
