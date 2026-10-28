import { initEngine, version as engineVersion, runCommand, status as engineStatus,
  startServer, stopServer, serverStatus } from 'libsirilohos.so';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { pasteboard } from '@kit.BasicServicesKit';

interface SequenceStatus {
  name: string;
  number: number;
  loaded: boolean;
}

interface EngineStatus {
  ready: boolean;
  version: string;
  cwd: string;
  image: string;
  sequence: SequenceStatus;
  threads?: number;
}

interface CommandResult {
  ok: boolean;
  code: number;
  output?: string;
  error?: string;
  ms?: number;
  status?: EngineStatus;
}

interface ServerStatus {
  running: boolean;
  bind: string;
  port: number;
  clients: number;
}

interface QuickAction {
  label: string;
  command: string;
}

const DATA_FILES: string[] = [
  'stars.csv',
  'constellations.csv',
  'constellationsnames.csv',
  'messier.csv',
  'ngc.csv',
  'ic.csv',
  'sh2.csv',
  'ldn.csv',
  'commands.json'
];

const QUICK_ACTIONS: QuickAction[] = [
  { label: '帮助', command: 'help' },
  { label: '状态', command: 'stat' },
  { label: '载入', command: 'load ' },
  { label: '转换', command: 'convert ' },
  { label: '校准', command: 'calibrate ' },
  { label: '配准', command: 'register ' },
  { label: '叠加', command: 'stack ' },
  { label: '拉伸', command: 'autostretch' },
  { label: '保存', command: 'save ' },
  { label: '目录', command: 'ls' }
];

@Entry
@Component
struct Index {
  @State private log: string = '';
  @State private input: string = '';
  @State private engineLine: string = '正在启动引擎…';
  @State private statusLine: string = '';
  @State private port: number = 4780;
  @State private serverOn: boolean = false;
  @State private lanExposed: boolean = false;
  @State private token: string = '';
  @State private ready: boolean = false;
  @State private busy: boolean = false;
  @State private version: string = '';

  private scroller: Scroller = new Scroller();
  private filesDir: string = '';
  private dataDir: string = '';
  private catalogPath: string = '';
  private timerId: number = -1;

  aboutToAppear(): void {
    this.bootstrap();
    this.timerId = setInterval(() => {
      this.refreshStatus();
    }, 2500);
  }

  aboutToDisappear(): void {
    if (this.timerId >= 0) {
      clearInterval(this.timerId);
      this.timerId = -1;
    }
  }

  private async bootstrap(): Promise<void> {
    const context = getContext(this) as common.UIAbilityContext;
    this.filesDir = context.filesDir;
    this.dataDir = `${this.filesDir}/usr/share/siril`;
    this.catalogPath = `${this.dataDir}/commands.json`;
    try {
      await this.installDataFiles(context);
      await this.installTestFrames(context);
      const rc = initEngine(this.filesDir, `${this.filesDir}/work`, this.catalogPath);
      hilog.info(0x5349, 'SIRIL', 'initEngine rc=%{public}d', rc);
      if (rc !== 0) {
        this.appendLog(`引擎初始化失败，错误码 ${rc}`);
        return;
      }
      this.version = engineVersion();
      this.ready = true;
      this.engineLine = `引擎就绪 · SIRIL ${this.version}`;
      this.appendLog(`SIRIL ${this.version} · HarmonyOS 嵌入式控制台`);
      this.appendLog('可用命令 218 条，输入 help 查看帮助，输入任意 SIRIL 命令执行。');
      this.startServer();
      await this.refreshStatus();
    } catch (error) {
      const err = error as BusinessError;
      hilog.error(0x5349, 'SIRIL', 'bootstrap failed: %{public}s', err.message);
      this.appendLog(`启动异常: ${err.message}`);
    }
  }

  private ensureDir(path: string): void {
    try {
      fs.mkdirSync(path, true);
    } catch (mkdirError) {
      // already present — nothing to do
    }
  }

  private async copyRaw(context: common.UIAbilityContext, rawPath: string, target: string): Promise<void> {
    const bytes = await context.resourceManager.getRawFileContent(rawPath);
    const payload = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
    const file = fs.openSync(target, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
    fs.writeSync(file.fd, payload);
    fs.closeSync(file);
  }

  private async installTestFrames(context: common.UIAbilityContext): Promise<void> {
    const frames: string[] = ['light_0001.fit', 'light_0002.fit', 'light_0003.fit', 'light_0004.fit', 'light_0005.fit'];
    const testDir = `${this.filesDir}/work/testrun`;
    this.ensureDir(testDir);
    // start every launch from a clean test directory so repeated runs cannot
    // accumulate converted frames (the UI is also the test fixture owner)
    try {
      const stale = fs.listFileSync(testDir);
      for (const name of stale) {
        try {
          fs.unlinkSync(`${testDir}/${name}`);
        } catch (unlinkError) {
          // ignore files the app cannot remove
        }
      }
    } catch (listError) {
      // empty directory
    }
    for (const frame of frames) {
      const target = `${testDir}/${frame}`;
      let present = false;
      try {
        present = fs.accessSync(target);
      } catch (probeError) {
        present = false;
      }
      if (present) {
        continue;
      }
      try {
        await this.copyRaw(context, `testdata/${frame}`, target);
        hilog.info(0x5349, 'SIRIL', 'staged test frame %{public}s', frame);
      } catch (copyError) {
        hilog.warn(0x5349, 'SIRIL', 'no bundled test frame %{public}s', frame);
        break;
      }
    }
  }

  private async installDataFiles(context: common.UIAbilityContext): Promise<void> {
    this.ensureDir(`${this.filesDir}/usr/share`);
    this.ensureDir(this.dataDir);
    this.ensureDir(`${this.filesDir}/work`);
    for (const name of DATA_FILES) {
      const target = `${this.dataDir}/${name}`;
      let present = false;
      try {
        present = fs.accessSync(target);
      } catch (probeError) {
        present = false;
      }
      if (present) {
        continue;
      }
      const bytes = await context.resourceManager.getRawFileContent(`siril-data/${name}`);
      const payload = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
      const file = fs.openSync(target, fs.OpenMode.CREATE | fs.OpenMode.READ_WRITE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, payload);
      fs.closeSync(file);
      hilog.info(0x5349, 'SIRIL', 'staged %{public}s (%{public}d bytes)', name, bytes.byteLength);
    }
  }

  private parseJson(text: string): ESObject {
    const value: ESObject = JSON.parse(text);
    return value;
  }

  private appendLog(text: string): void {
    const stamp = new Date().toTimeString().slice(0, 8);
    const line = text.length > 0 ? text : '(无输出)';
    this.log = `${this.log}[${stamp}] ${line}\n`;
    if (this.log.length > 60000) {
      this.log = this.log.slice(this.log.length - 50000);
    }
    setTimeout(() => {
      this.scroller.scrollEdge(Edge.Bottom);
    }, 30);
  }

  private async runCommand(command: string): Promise<void> {
    const cmd = command.trim();
    if (cmd.length === 0) {
      return;
    }
    if (!this.ready) {
      this.appendLog('引擎尚未就绪');
      return;
    }
    if (this.busy) {
      this.appendLog('上一条命令仍在执行，请稍候');
      return;
    }
    this.busy = true;
    this.appendLog(`> ${cmd}`);
    try {
      const raw = await runCommand(cmd);
      const result = this.parseJson(raw) as CommandResult;
      const body = result.ok ? (result.output ?? '') : `${result.error ?? '命令失败'}\n${result.output ?? ''}`;
      this.appendLog(body.length > 0 ? body : (result.ok ? '(完成)' : '(失败)'));
      if (result.status) {
        this.applyStatus(result.status);
      }
    } catch (error) {
      const err = error as BusinessError;
      this.appendLog(`执行异常: ${err.message}`);
    } finally {
      this.busy = false;
    }
  }

  private applyStatus(status: EngineStatus): void {
    const parts: string[] = [];
    parts.push(`目录 ${status.cwd || '-'}`);
    if (status.image && status.image.length > 0) {
      parts.push(`图像 ${status.image}`);
    }
    if (status.sequence && status.sequence.loaded) {
      parts.push(`序列 ${status.sequence.name} (${status.sequence.number})`);
    }
    if (status.threads) {
      parts.push(`线程 ${status.threads}`);
    }
    this.statusLine = parts.join('   ·   ');
  }

  private async refreshStatus(): Promise<void> {
    if (!this.ready) {
      return;
    }
    try {
      const status = this.parseJson(engineStatus()) as EngineStatus;
      this.applyStatus(status);
      const server = this.parseJson(serverStatus()) as ServerStatus;
      this.serverOn = server.running;
    } catch (error) {
      // keep previous values; a malformed snapshot must not break the console
    }
  }

  private startServer(): void {
    const bind = this.lanExposed ? '0.0.0.0' : '127.0.0.1';
    const rc = startServer(bind, this.port, this.token);
    hilog.info(0x5349, 'SIRIL', 'startServer rc=%{public}d bind=%{public}s port=%{public}d', rc, bind, this.port);
    if (rc === 0) {
      this.serverOn = true;
      if (this.lanExposed) {
        const guarded = this.token.length > 0 ? '已启用令牌' : '未启用令牌';
        this.appendLog(`控制端口已监听 ${bind}:${this.port}（局域网可访问，${guarded}）`);
      } else {
        this.appendLog(`控制端口已监听 ${bind}:${this.port}（JSON 行协议，hdc fport tcp:${this.port} tcp:${this.port}）`);
      }
    } else {
      this.serverOn = false;
      this.appendLog(`控制端口启动失败，错误码 ${rc}`);
    }
  }

  private stopServer(): void {
    stopServer();
    this.serverOn = false;
    this.appendLog('控制端口已停止');
  }

  private copyEndpoint(): void {
    const text = `hdc fport tcp:${this.port} tcp:${this.port}`;
    const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
    pasteboard.getSystemPasteboard().setData(data).then(() => {
      this.appendLog(`已复制端口转发命令: ${text}`);
    });
  }

  @Builder chip(label: string, active: boolean) {
    Text(label)
      .fontSize(12)
      .fontColor(active ? '#0B1020' : '#9FB3D9')
      .backgroundColor(active ? '#6FD3C7' : '#1B2540')
      .borderRadius(10)
      .padding({ left: 10, right: 10, top: 4, bottom: 4 })
  }

  build() {
    Column() {
      Row() {
        Column() {
          Text('SIRIL')
            .fontSize(30)
            .fontWeight(FontWeight.Bold)
            .fontColor('#EAF2FF')
          Text('天文图像处理 · HarmonyOS PC')
            .fontSize(12)
            .fontColor('#7C8DB5')
            .margin({ top: 2 })
        }
        .alignItems(HorizontalAlign.Start)

        Blank()

        Row({ space: 8 }) {
          this.chip(this.ready ? `引擎 ${this.version}` : '引擎启动中', this.ready)
          this.chip(this.serverOn ? `端口 ${this.port}` : '端口关闭', this.serverOn)
          if (this.busy) {
            this.chip('执行中', false)
          }
        }
      }
      .width('100%')
      .padding({ left: 20, right: 20, top: 14, bottom: 10 })

      Text(this.statusLine.length > 0 ? this.statusLine : this.engineLine)
        .fontSize(11)
        .fontColor('#6C7FA8')
        .width('100%')
        .padding({ left: 20, right: 20, bottom: 8 })
        .maxLines(2)
        .textOverflow({ overflow: TextOverflow.Ellipsis })

      Scroll(this.scroller) {
        Text(this.log.length > 0 ? this.log : '等待命令…')
          .fontSize(13)
          .fontColor('#CFE0FF')
          .fontFamily('HarmonyOS Sans Mono')
          .lineHeight(19)
          .width('100%')
          .textAlign(TextAlign.Start)
          .copyOption(CopyOptions.LocalDevice)
          .padding(14)
      }
      .scrollBar(BarState.Auto)
      .backgroundColor('#0D1424')
      .borderRadius(14)
      .border({ width: 1, color: '#1D2A47' })
      .layoutWeight(1)
      .width('100%')
      .margin({ left: 16, right: 16 })

      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        ForEach(QUICK_ACTIONS, (action: QuickAction) => {
          Button(action.label)
            .fontSize(12)
            .height(30)
            .backgroundColor('#18233C')
            .fontColor('#B9C9EA')
            .margin({ right: 6, top: 8 })
            .onClick(() => {
              if (action.command.endsWith(' ')) {
                this.input = action.command;
              } else {
                this.runCommand(action.command);
              }
            })
        }, (action: QuickAction) => action.label)
      }
      .width('100%')
      .padding({ left: 16, right: 16 })

      Row({ space: 10 }) {
        TextInput({ text: this.input, placeholder: '输入 SIRIL 命令，例如 load m31.fit 或 stack r_pp_light -out=result' })
          .layoutWeight(1)
          .height(42)
          .fontSize(13)
          .backgroundColor('#111A2D')
          .fontColor('#E3ECFF')
          .placeholderColor('#5B6B8C')
          .caretColor('#6FD3C7')
          .onChange((value: string) => {
            this.input = value;
          })
          .onSubmit(() => {
            const cmd = this.input;
            this.input = '';
            this.runCommand(cmd);
          })
        Button(this.busy ? '执行中' : '运行')
          .height(42)
          .width(84)
          .fontSize(14)
          .enabled(!this.busy)
          .backgroundColor(this.busy ? '#2A3550' : '#2E7DF7')
          .onClick(() => {
            const cmd = this.input;
            this.input = '';
            this.runCommand(cmd);
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 10 })

      Row({ space: 10 }) {
        Text('控制端口')
          .fontSize(12)
          .fontColor('#8FA2C8')
        TextInput({ text: this.port.toString() })
          .type(InputType.Number)
          .width(90)
          .height(34)
          .fontSize(12)
          .backgroundColor('#111A2D')
          .fontColor('#E3ECFF')
          .enabled(!this.serverOn)
          .onChange((value: string) => {
            const parsed = Number.parseInt(value, 10);
            if (!Number.isNaN(parsed) && parsed > 0 && parsed < 65536) {
              this.port = parsed;
            }
          })
        Button(this.serverOn ? '停止监听' : '启动监听')
          .height(34)
          .fontSize(12)
          .backgroundColor(this.serverOn ? '#3A2033' : '#1D3B36')
          .fontColor(this.serverOn ? '#F2A6C8' : '#7FE3D2')
          .onClick(() => {
            if (this.serverOn) {
              this.stopServer();
            } else {
              this.startServer();
            }
          })
        Button('复制转发命令')
          .height(34)
          .fontSize(12)
          .backgroundColor('#18233C')
          .fontColor('#B9C9EA')
          .onClick(() => {
            this.copyEndpoint();
          })
        Blank()
        Button('清空日志')
          .height(34)
          .fontSize(12)
          .backgroundColor('#18233C')
          .fontColor('#B9C9EA')
          .onClick(() => {
            this.log = '';
          })
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 8 })

      Row({ space: 10 }) {
        Text('局域网暴露')
          .fontSize(12)
          .fontColor('#8FA2C8')
        Toggle({ type: ToggleType.Switch, isOn: this.lanExposed })
          .enabled(!this.serverOn)
          .selectedColor('#2E7DF7')
          .onChange((value: boolean) => {
            this.lanExposed = value;
          })
        TextInput({ text: this.token, placeholder: '访问令牌（可选，局域网建议设置）' })
          .layoutWeight(1)
          .height(34)
          .fontSize(12)
          .enabled(!this.serverOn)
          .backgroundColor('#111A2D')
          .fontColor('#E3ECFF')
          .placeholderColor('#5B6B8C')
          .onChange((value: string) => {
            this.token = value;
          })
        Text(this.lanExposed ? '0.0.0.0' : '127.0.0.1')
          .fontSize(11)
          .fontColor('#5B6B8C')
      }
      .width('100%')
      .padding({ left: 16, right: 16, top: 2, bottom: 14 })
    }
    .width('100%')
    .height('100%')
    .backgroundColor('#080D18')
  }
}
