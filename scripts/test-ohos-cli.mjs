#!/usr/bin/env node
/*
 * End-to-end smoke test for the SIRIL HarmonyOS application.
 *
 * Requires a running app on a hdc-visible device.  The script forwards the
 * control port, exercises the transport, the catalog and a real FITS pipeline
 * (convert → register → stack → load → stat), then prints a summary.
 *
 *   node scripts/test-ohos-cli.mjs --device 192.168.1.56:43975
 */

import { execFileSync } from 'node:child_process';
import { createConnection } from 'node:net';
import process from 'node:process';

const DEFAULT_HDC = process.env.HDC ||
  '/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc';

function parseArgs(argv) {
  const options = { port: 4780, host: '127.0.0.1', timeout: 180000 };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--device') options.device = argv[++i];
    else if (arg === '--port') options.port = Number(argv[++i]);
    else if (arg === '--host') options.host = argv[++i];
    else if (arg === '--hdc') options.hdc = argv[++i];
    else if (arg === '--help' || arg === '-h') {
      console.log('usage: test-ohos-cli.mjs [--device serial] [--port 4780] [--hdc path]');
      process.exit(0);
    } else {
      throw new Error(`unknown argument ${arg}`);
    }
  }
  return options;
}

class Client {
  constructor(port, host) {
    this.port = port;
    this.host = host;
    this.pending = new Map();
    this.nextId = 1;
    this.buffer = '';
  }

  connect() {
    return new Promise((resolve, reject) => {
      this.socket = createConnection({ host: this.host, port: this.port });
      this.socket.setEncoding('utf8');
      this.socket.once('connect', resolve);
      this.socket.once('error', reject);
      this.socket.on('data', (chunk) => {
        this.buffer += chunk;
        let index = this.buffer.indexOf('\n');
        while (index >= 0) {
          const line = this.buffer.slice(0, index).trim();
          this.buffer = this.buffer.slice(index + 1);
          if (line.length > 0) {
            const message = JSON.parse(line);
            const entry = this.pending.get(message.id);
            if (entry) {
              this.pending.delete(message.id);
              entry(message);
            }
          }
          index = this.buffer.indexOf('\n');
        }
      });
    });
  }

  request(op, extra = {}) {
    const id = String(this.nextId++);
    return new Promise((resolve) => {
      this.pending.set(id, resolve);
      this.socket.write(`${JSON.stringify({ id, op, ...extra })}\n`);
    });
  }

  close() {
    this.socket?.end();
  }
}

function hdcRun(hdc, args) {
  return execFileSync(hdc, args, { stdio: ['ignore', 'pipe', 'pipe'], encoding: 'utf8' });
}

function launch(serial, hdc) {
  const run = hdc || DEFAULT_HDC;
  try {
    hdcRun(run, ['-t', serial, 'shell', 'aa', 'force-stop', 'com.joinother.siril']);
  } catch {
    /* not running */
  }
  hdcRun(run, ['-t', serial, 'shell', 'aa', 'start', '-a', 'EntryAbility', '-b', 'com.joinother.siril']);
  const deadline = Date.now() + 20000;
  while (Date.now() < deadline) {
    const output = hdcRun(run, ['-t', serial, 'shell', 'pidof', 'com.joinother.siril']).trim();
    if (output.length > 0) return;
    execFileSync('/bin/sleep', ['0.5']);
  }
  throw new Error('application did not start');
}

function forward(options) {
  if (!options.device) return;
  const hdc = options.hdc || DEFAULT_HDC;
  try {
    execFileSync(hdc, ['-t', options.device, 'fport', 'rm', `tcp:${options.port}`, `tcp:${options.port}`],
      { stdio: 'ignore' });
  } catch {
    /* no previous rule */
  }
  execFileSync(hdc, ['-t', options.device, 'fport', `tcp:${options.port}`, `tcp:${options.port}`],
    { stdio: ['ignore', 'pipe', 'pipe'] });
}

const results = [];
function check(name, ok, detail = '') {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  — ${detail}` : ''}`);
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  if (options.device) {
    launch(options.device, options.hdc);
    execFileSync('/bin/sleep', ['3']);
  }
  forward(options);

  const client = new Client(options.port, options.host);
  await client.connect();

  const ping = await client.request('ping');
  check('transport: ping', ping.ok === true);

  const status = await client.request('status');
  check('engine: status', status.ok && status.status?.ready === true,
    `version ${status.status?.version}`);

  const catalog = await client.request('catalog');
  const commands = catalog.commands || [];
  check('catalog: 200+ commands', commands.length >= 200, `${commands.length} commands`);
  const hasStack = commands.some((entry) => entry.name === 'stack');
  check('catalog: stack present', hasStack);

  const help = await client.request('exec', { cmd: 'help' });
  check('engine: help', help.ok && help.output.includes('LIST OF AVAILABLE COMMANDS'));

  const chdir = await client.request('exec', {
    cmd: `cd ${status.status.cwd}/testrun`,
  });
  check('engine: cd working directory', chdir.ok === true);

  const convert = await client.request('exec', { cmd: 'convert light' });
  check('pipeline: convert', convert.ok && /5 file\(s\) created/.test(convert.output));

  const register = await client.request('exec', { cmd: 'register light' });
  check('pipeline: register', register.ok && /5 registered/.test(register.output));

  const stack = await client.request('exec', { cmd: 'stack r_light_ rej 3 3 -out=stacked' });
  check('pipeline: stack', stack.ok && /Stacked sequence successfully/.test(stack.output));

  const load = await client.request('exec', { cmd: 'load stacked' });
  check('pipeline: load', load.ok);

  const stat = await client.request('exec', { cmd: 'stat' });
  check('pipeline: stat', stat.ok && /Median/.test(stat.output));

  const png = await client.request('exec', { cmd: 'savepng autotest' });
  check('pipeline: savepng', png.ok && /Saving PNG/.test(png.output));

  const bad = await client.request('exec', { cmd: 'definitely_not_a_command' });
  check('error: unknown command rejected', bad.ok === false);

  client.close();

  const failed = results.filter((entry) => !entry.ok);
  console.log(`\n${results.length - failed.length}/${results.length} checks passed`);
  if (failed.length > 0) process.exitCode = 1;
}

main().catch((error) => {
  console.error(`error: ${error.message}`);
  process.exit(1);
});
