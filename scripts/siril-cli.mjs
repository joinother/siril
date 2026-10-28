#!/usr/bin/env node
/*
 * SIRIL HarmonyOS CLI client.
 *
 * Talks to the control port exposed by the SIRIL HarmonyOS application.  The
 * protocol is one JSON object per line:
 *
 *   request : {"id":"1","op":"exec","cmd":"load m31.fit"}
 *   reply   : {"id":"1","ok":true,"code":0,"output":"...","status":{...}}
 *
 * Examples
 *   node scripts/siril-cli.mjs --list
 *   node scripts/siril-cli.mjs --command "help"
 *   node scripts/siril-cli.mjs --device 192.168.1.56:43975 --command "stat"
 *   node scripts/siril-cli.mjs --interactive
 */

import { execFileSync } from 'node:child_process';
import { createConnection } from 'node:net';
import { readFileSync } from 'node:fs';
import { createInterface } from 'node:readline';
import process from 'node:process';

const DEFAULT_PORT = 4780;
const DEFAULT_HOST = '127.0.0.1';
const DEFAULT_HDC = process.env.HDC ||
  '/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc';

function usage(exitCode = 0) {
  console.log(`用法：
  siril-cli.mjs --command <SIRIL命令> [选项]
  siril-cli.mjs --list [选项]
  siril-cli.mjs --describe <命令> [选项]
  siril-cli.mjs --batch <JSON文件> [选项]
  siril-cli.mjs --script <SIRIL脚本文件> [选项]
  siril-cli.mjs --interactive [选项]
  siril-cli.mjs --status [选项]

选项：
  --port <端口>         控制端口，默认 ${DEFAULT_PORT}
  --host <地址>         默认 ${DEFAULT_HOST}
  --device <设备ID>     通过 hdc 建立端口转发（hdc list targets）
  --hdc <路径>          默认 DevEco 自带 hdc
  --token <令牌>        控制端口令牌（应用默认不启用）
  --timeout <毫秒>      单条命令超时，默认 120000
  --json                只输出原始 JSON
  --quiet               不打印命令回显
  --help                显示帮助

示例：
  node scripts/siril-cli.mjs --command help
  node scripts/siril-cli.mjs --command "stat"
  node scripts/siril-cli.mjs --device 192.168.1.56:43975 --command "cd /data/storage/el2/base/files/work"
  node scripts/siril-cli.mjs --batch commands.json`);
  process.exit(exitCode);
}

function parseArgs(argv) {
  const options = {
    host: DEFAULT_HOST,
    port: DEFAULT_PORT,
    timeout: 120000,
    json: false,
    quiet: false
  };
  const keys = {
    '--host': 'host',
    '--port': 'port',
    '--device': 'device',
    '--hdc': 'hdc',
    '--token': 'token',
    '--timeout': 'timeout',
    '--command': 'command',
    '--payload': 'payload',
    '--batch': 'batch',
    '--script': 'script',
    '--describe': 'describe'
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--help' || arg === '-h') usage(0);
    if (arg === '--json') { options.json = true; continue; }
    if (arg === '--quiet') { options.quiet = true; continue; }
    if (arg === '--list') { options.list = true; continue; }
    if (arg === '--status') { options.status = true; continue; }
    if (arg === '--interactive') { options.interactive = true; continue; }
    const key = keys[arg];
    if (key === undefined || i + 1 >= argv.length) {
      throw new Error(`未知或缺少参数：${arg}`);
    }
    options[key] = argv[++i];
  }
  options.port = Number(options.port);
  if (!Number.isInteger(options.port) || options.port <= 0 || options.port > 65535) {
    throw new Error('--port 必须是 1-65535 的整数');
  }
  options.timeout = Number(options.timeout);
  if (!Number.isFinite(options.timeout) || options.timeout < 500) {
    throw new Error('--timeout 至少为 500');
  }
  const modes = Number(Boolean(options.list)) + Number(Boolean(options.status)) +
    Number(Boolean(options.describe !== undefined)) + Number(Boolean(options.batch)) +
    Number(Boolean(options.script)) + Number(Boolean(options.interactive)) +
    Number(Boolean(options.command));
  if (modes === 0) usage(1);
  return options;
}

function forwardPort(options) {
  if (!options.device) return;
  const hdc = options.hdc || DEFAULT_HDC;
  try {
    execFileSync(hdc, ['-t', options.device, 'fport', `tcp:${options.port}`, `tcp:${options.port}`],
      { stdio: ['ignore', 'pipe', 'pipe'] });
  } catch (error) {
    const detail = error.stderr ? error.stderr.toString().trim() : error.message;
    throw new Error(`hdc 端口转发失败：${detail}`);
  }
}

class ControlClient {
  constructor(options) {
    this.options = options;
    this.socket = null;
    this.buffer = '';
    this.pending = new Map();
    this.nextId = 1;
  }

  connect() {
    return new Promise((resolve, reject) => {
      const socket = createConnection({ host: this.options.host, port: this.options.port });
      socket.setEncoding('utf8');
      socket.once('connect', () => {
        this.socket = socket;
        resolve();
      });
      socket.once('error', (error) => reject(new Error(`无法连接 ${this.options.host}:${this.options.port} — ${error.message}`)));
      socket.on('data', (chunk) => this.onData(chunk));
      socket.on('close', () => {
        for (const { reject: fail } of this.pending.values()) {
          fail(new Error('控制端口连接已关闭'));
        }
        this.pending.clear();
      });
    });
  }

  onData(chunk) {
    this.buffer += chunk;
    let index = this.buffer.indexOf('\n');
    while (index >= 0) {
      const line = this.buffer.slice(0, index).trim();
      this.buffer = this.buffer.slice(index + 1);
      if (line.length > 0) this.handleLine(line);
      index = this.buffer.indexOf('\n');
    }
  }

  handleLine(line) {
    let message;
    try {
      message = JSON.parse(line);
    } catch (error) {
      for (const { reject } of this.pending.values()) reject(new Error(`无法解析响应：${line.slice(0, 200)}`));
      this.pending.clear();
      return;
    }
    const entry = this.pending.get(String(message.id));
    if (!entry) return;
    this.pending.delete(String(message.id));
    entry.resolve(message);
  }

  request(op, extra = {}) {
    if (!this.socket) throw new Error('尚未连接');
    const id = String(this.nextId++);
    const payload = { id, op, ...extra };
    if (this.options.token) payload.token = this.options.token;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`命令超时（${this.options.timeout}ms）`));
      }, this.options.timeout);
      this.pending.set(id, {
        resolve: (value) => { clearTimeout(timer); resolve(value); },
        reject: (error) => { clearTimeout(timer); reject(error); }
      });
      this.socket.write(`${JSON.stringify(payload)}\n`);
    });
  }

  close() {
    if (this.socket) this.socket.end();
    this.socket = null;
  }
}

function printResult(response, options, command) {
  if (options.json) {
    console.log(JSON.stringify(response));
    return;
  }
  if (command && !options.quiet) console.log(`> ${command}`);
  if (response.output) process.stdout.write(response.output.endsWith('\n') ? response.output : `${response.output}\n`);
  if (!response.ok) {
    console.error(`命令失败：${response.error || `code ${response.code}`}`);
  }
}

function printStatus(response) {
  const status = response.status || {};
  const lines = [
    `引擎版本   ${status.version ?? '-'}`,
    `工作目录   ${status.cwd ?? '-'}`,
    `当前图像   ${status.image && status.image.length > 0 ? status.image : '-'}`,
    `当前序列   ${status.sequence && status.sequence.loaded ? `${status.sequence.name} (${status.sequence.number})` : '-'}`,
    `线程数     ${status.threads ?? '-'}`
  ];
  console.log(lines.join('\n'));
}

function describe(commands, name) {
  const entry = commands.find((c) => c.name === name);
  if (!entry) {
    console.error(`未知命令：${name}（共 ${commands.length} 条）`);
    process.exitCode = 1;
    return;
  }
  const required = entry.requires.length > 0 ? entry.requires.join(', ') : '无';
  console.log(`${entry.name}\n  用法     ${entry.usage}\n  最少参数 ${entry.min_args}\n  可脚本化 ${entry.scriptable ? '是' : '否'}\n  前置条件 ${required}`);
}

async function runScript(client, path, options) {
  const text = readFileSync(path, 'utf8');
  const lines = text.split(/\r?\n/).map((line) => line.trim()).filter((line) => line.length > 0 && !line.startsWith('#'));
  for (const command of lines) {
    const response = await client.request('exec', { cmd: command });
    printResult(response, options, command);
    if (!response.ok) {
      process.exitCode = 1;
      break;
    }
  }
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  forwardPort(options);

  const client = new ControlClient(options);
  await client.connect();

  try {
    if (options.status) {
      const response = await client.request('status');
      if (options.json) console.log(JSON.stringify(response));
      else printStatus(response);
      return;
    }

    if (options.list || options.describe !== undefined) {
      const response = await client.request('catalog');
      const commands = response.commands || [];
      if (options.json) console.log(JSON.stringify(commands));
      else if (options.list) {
        for (const entry of commands) console.log(`${entry.name.padEnd(22)} ${entry.usage}`);
        console.log(`\n共 ${commands.length} 条命令`);
      } else {
        describe(commands, options.describe);
      }
      return;
    }

    if (options.batch) {
      const items = JSON.parse(readFileSync(options.batch, 'utf8'));
      for (const item of items) {
        const command = item.payload === undefined ? item.command : `${item.command} ${item.payload}`;
        const response = await client.request('exec', { cmd: command });
        printResult(response, options, command);
        if (!response.ok) process.exitCode = 1;
      }
      return;
    }

    if (options.script) {
      await runScript(client, options.script, options);
      return;
    }

    if (options.interactive) {
      const rl = createInterface({ input: process.stdin, output: process.stdout, prompt: 'siril> ' });
      rl.prompt();
      for await (const line of rl) {
        const command = line.trim();
        if (command === 'exit' || command === 'quit') break;
        if (command.length > 0) {
          const response = await client.request('exec', { cmd: command });
          printResult(response, options, command);
        }
        rl.prompt();
      }
      rl.close();
      return;
    }

    const command = options.payload === undefined ? options.command : `${options.command} ${options.payload}`;
    const response = await client.request('exec', { cmd: command });
    printResult(response, options, command);
    if (!response.ok) process.exitCode = 1;
  } finally {
    client.close();
  }
}

main().catch((error) => {
  console.error(`错误：${error.message}`);
  process.exit(1);
});
