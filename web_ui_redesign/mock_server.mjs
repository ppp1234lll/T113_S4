#!/usr/bin/env node
/* IVSBox Web 界面重设计 —— 本地预览服务器（零依赖，Node ≥ 18）
 * =========================================================================
 * 用途：板子（192.168.2.120）不在线时，在 PC 上**原样跑真前端代码**并喂
 *       模拟数据，用于：① 截取现有界面的"现状图"；② 预览重设计原型。
 *       前端文件不改一字（直接服务 ivsbox/web/static/），只模拟 /api/*。
 *
 * 启动：node mock_server.mjs          （端口 8020）
 *   http://127.0.0.1:8020/            —— 现有界面（真实 static 文件）
 *   http://127.0.0.1:8020/prototype   —— 重设计原型 prototype.html
 *
 * 登录：admin / 88888888（出厂默认，must_change=false，直接进主界面）
 *
 * ⚠ 本文件只服务于界面重设计与验收，不是产品代码；数据全是编造的演示值。
 */
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const STATIC_DIR = join(HERE, '..', 'ivsbox', 'web', 'static');
const PORT = 8020;

const MIME = {
  '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.json': 'application/json; charset=utf-8',
  '.ico': 'image/x-icon', '.txt': 'text/plain; charset=utf-8',
};

/* ==================== 模拟数据 ==================== */
const now = () => Math.floor(Date.now() / 1000);
const BOOT_MS = Date.now() - (3 * 86400 + 2 * 3600 + 41 * 60) * 1000; // 开机于 3 天前

const ok = (d) => ({ ok: true, d });
const fail = (msg, e = -1) => ({ ok: false, msg, e });

const overview = () => ({
  version: '1.4.0',
  uptime_ms: Date.now() - BOOT_MS,
  device: {
    version: '1.4.0', model: 'TQ113-S4 EVB1', soc: 'Allwinner T113-S4',
    sn: '1', sn_source: 'default',          /* 出厂默认 SN：前端显示橙色告警 */
    id_mac: '02:1c:6b:8a:33:d0', eth0_mac: '0e:2a:5f:44:c1:08',
    hostname: 'TQ113', kernel: '5.4.61', machine: 'armv7l',
    wall_sec: now(),
  },
  net: {
    state: 'WIRED_UP', active_wan: 'wired', mn: [1, 1],
    link: {
      gateway: '192.168.2.1', gateway_if: 'eth0',
      ifaces: [
        { name: 'eth0', present: true, up: true, ip: '192.168.2.120', mask: '255.255.255.0', mac: '0e:2a:5f:44:c1:08' },
        { name: 'usb0', present: true, up: true, ip: '10.47.158.22', mask: '255.255.255.248', mac: '1a:2b:3c:4d:5e:6f' },
      ],
    },
  },
  cameras: { mode: 1, cns: [1, 1, 2, 1, 4, 1] },   /* 3 号故障、5 号延时，便于展示状态色 */
});

const gps = () => {
  const t = new Date();
  return {
    fix: { valid: true, lon: 114.057868, lat: 22.543099, alt: 58.3, speed_kph: 12.4, course_deg: 273.5, satellites: 11, hdop: 0.9, fix_quality: 1 },
    utc: { have: true, year: t.getUTCFullYear(), month: t.getUTCMonth() + 1, day: t.getUTCDate(), hour: t.getUTCHours(), minute: t.getUTCMinutes(), second: t.getUTCSeconds() },
    link: { path: '/dev/ttyS1', open: true, rx_bytes: 1246860, age_ms: 1200 },
    stats: { sentences_ok: 41233, fix_updates: 12045, checksum_bad: 2, malformed: 0, too_long: 0, noise_bytes: 341, time_sync_ok: 1180, time_sync_rejected: 3 },
  };
};

const board = () => ({
  link: {
    path: '/dev/ttyS2', open: true, up: true, age_ms: 2300,
    frames: 18234, resets: 1, crc_err: 2, tail_err: 0, too_long: 0, noise: 1543,
    rx_bytes: 1246800, tx_bytes: 98234, serial_err: 0, tx_drop: 0,
    polls: 9102, reports: 1440, heartbeats: 2880, query_rsp: 5220, queue: 0, queue_full: 0,
  },
  data: { V: 23.6, A: 1.42, T: 32.5, H: 46.8, DS: 2, APOWER: 33.5, AKW: 126.4, P: '-1', SPD: '0', PA: '1', PV: '1' },
  limit: { hv: 260, lv: 180, ov: 10, tu: 60, tl: -10, hu: 90, hl: 20, sl: 30, ld: 5 },
  relay: [
    { state: 1, v: 23.6, a: 0.82, power: 19.4, elec: 48.2 },
    { state: 2, v: null, a: null, power: null, elec: 51.7 },
    { state: 1, v: 23.5, a: 0.31, power: 7.3, elec: 26.5 },
  ],
  door_events: 7,
});

const devices = () => ({
  count: 3,
  items: [
    { brand: 'Hikvision', ip: '192.168.2.201', mac: '44:47:cc:11:22:33', model: 'DS-2CD3T46WD', serial: 'D72135482', port: 554, found_by: 'ONVIF' },
    { brand: 'Hikvision', ip: '192.168.2.202', mac: '44:47:cc:11:22:44', model: 'DS-2DE4223IW', serial: 'D72135483', port: 554, found_by: 'ONVIF' },
    { brand: 'Dahua', ip: '192.168.2.203', mac: '3c:ef:8c:aa:bb:55', model: 'DH-IPC-HFW2449M', serial: '4L08C9PAG', port: 554, found_by: '端口扫描' },
  ],
});

const snmp = (which) => which === 'cam' ? {
  state: 2,
  slots: [
    { name: 'sysDescr', oid: '1.3.6.1.2.1.1.1.0', n_rows: 1, rows: [{ str: 'IPC_HX5X 4.0.build 211116' }] },
    { name: 'sysUpTime', oid: '1.3.6.1.2.1.1.3.0', n_rows: 1, rows: [{ str: '18天 4:32:11' }] },
    { name: 'cpuPercent', oid: '1.3.6.1.4.1.39165.1.7.0', n_rows: 1, rows: [{ num: 23 }] },
    { name: 'memUsed', oid: '1.3.6.1.4.1.39165.1.11.0', n_rows: 1, rows: [{ num: 41 }] },
  ],
} : { state: 0, slots: [] };

/* 配置默认值表：与 ivsbox/src/modules/config/iv_config.c 的 s_defaults 同源（节选），
 * d 字段＝中文说明，t 字段＝类型（前端 /api/config 只用值本身与 defaults 的 d）。 */
const CH_LIST = Array.from({ length: 8 }, (_, i) => i);
const configItems = {
  sys: {
    'sys.debug.mode': false, 'sys.reboot.hour': -1, 'sys.report.interval_s': 300,
    'sys.transport.mode': 4, 'sys.sample.period_ms': 1000,
    'sys.tunnel.server.host': '47.104.98.214', 'sys.tunnel.server.port': 7000,
    'sys.tunnel.ttl_s': 3600, 'sys.tunnel.web.port': 80,
    'sys.tunnel.media.port': 18000, 'sys.tunnel.cam.port': 8081,
    'sys.tunnel.cam.enable': false,
  },
  net: {
    'net.ip': '', 'net.mask': '', 'net.gateway': '',
    'net.server.wired.host': '47.104.250.225', 'net.server.wired.port': 6012,
    'net.server.wireless.host': '47.104.250.225', 'net.server.wireless.port': 6012,
    'net.server.upgrade.host': '47.104.98.214', 'net.server.upgrade.port': 8989,
    'net.server.log.host': '', 'net.server.log.port': 0,
  },
  probe: {
    'probe.target1': '47.104.250.225', 'probe.target2': '114.114.114.114',
    'probe.interval_ms': 5000, 'probe.timeout_ms': 2000, 'probe.max_fail': 3,
  },
  camera: Object.assign(
    {
      'camera.search': 'off', 'camera.detect.mode': 1, 'camera.detect.rtt_ms': 200,
      'camera.snmp.enable': true, 'camera.snmp.community': 'public',
      'camera.snmp.timeout_ms': 1500, 'camera.snmp.retries': 1,
      'camera.snmp.oid.sysDescr': '1.3.6.1.2.1.1.1.0',
      'camera.snmp.oid.sysUpTime': '1.3.6.1.2.1.1.3.0',
      'camera.snmp.oid.sysName': '1.3.6.1.2.1.1.5.0',
      'camera.snmp.oid.deviceType': '1.3.6.1.4.1.39165.1.1.0',
      'camera.snmp.oid.softVersion': '1.3.6.1.4.1.39165.1.3.0',
      'camera.snmp.oid.serialNumber': '1.3.6.1.4.1.39165.1.5.0',
      'camera.snmp.oid.cpuPercent': '1.3.6.1.4.1.39165.1.7.0',
      'camera.snmp.oid.memUsed': '1.3.6.1.4.1.39165.1.11.0',
      'camera.snmp.oid.devStatus': '',
      'camera.rec.hold_ms': 10000, 'camera.rec.debounce_ms': 60000,
    },
    ...CH_LIST.map((i) => {
      const filled = i < 6 ? {
        vendor: i % 2 ? 'Dahua' : 'Hikvision', ip: `192.168.2.20${i + 1}`,
        mac: `44:47:cc:11:22:${33 + i}`, user: 'admin', pass: '', record: i < 2,
        stream: 'main', url: '',
      } : { vendor: '', ip: '', mac: '', user: '', pass: '', record: false, stream: '', url: '' };
      const o = {};
      for (const [k, v] of Object.entries(filled)) o[`camera.ch.${i}.${k}`] = v;
      return o;
    }),
  ),
  switch: {
    'switch.ip': '192.168.2.2', 'switch.vendor': 'H3C', 'switch.model': 'S5130S-28P',
    'switch.snmp.enable': true, 'switch.snmp.port': 161, 'switch.snmp.community': 'public',
    'switch.snmp.timeout_ms': 1500, 'switch.snmp.retries': 1,
    'switch.snmp.oid.sysDescr': '1.3.6.1.2.1.1.1.0',
    'switch.snmp.oid.sysUpTime': '1.3.6.1.2.1.1.3.0',
    'switch.snmp.oid.ifDescr': '1.3.6.1.2.1.2.2.1.2',
    'switch.snmp.oid.ifSpeed': '1.3.6.1.2.1.2.2.1.5',
    'switch.snmp.oid.ifOperStatus': '1.3.6.1.2.1.2.2.1.8',
    'switch.snmp.oid.ifInOctets': '1.3.6.1.2.1.2.2.1.10',
    'switch.snmp.oid.ifOutOctets': '1.3.6.1.2.1.2.2.1.16',
  },
  elec: {
    'elec.volt_high': 260.0, 'elec.volt_low': 180.0, 'elec.over_current': 10.0,
    'elec.leak_current': 5.0, 'elec.power': 100.0,
  },
  netthr: {
    'netthr.timeout_ms': 2000, 'netthr.loss_count': 3,
    'netthr.reboot_max': 0, 'netthr.reboot_interval_s': 60,
  },
  sensor: {
    'sensor.tilt': 30.0, 'sensor.temp_high': 60.0, 'sensor.temp_low': -10.0,
    'sensor.humi_high': 90.0, 'sensor.humi_low': 20.0,
  },
};

/* 中文说明（defaults=1 时每个键的 d）：与上面 configItems 键一一对应。 */
const descs = {
  'sys.debug.mode': '调试模式（开＝保留调试口与详细日志）',
  'sys.reboot.hour': '定时重启时间（每天几点，0-23；-1＝不重启）',
  'sys.report.interval_s': '定时上报间隔（秒）',
  'sys.transport.mode': '传输模式：1=有线 2=无线 3=有线+无线 4=自动选择',
  'sys.sample.period_ms': '采集周期（毫秒）',
  'sys.tunnel.server.host': 'frps 地址（空＝不开通道；受控远程通道的服务端）',
  'sys.tunnel.server.port': 'frps 端口（0＝默认 7000）',
  'sys.tunnel.ttl_s': '通道默认有效期（秒；单次开启可用 ttl 覆写，上限 86400）',
  'sys.tunnel.web.port': '本机 Web 服务端口',
  'sys.tunnel.media.port': '本机媒体服务端口',
  'sys.tunnel.cam.port': '摄像机受控代理端口',
  'sys.tunnel.cam.enable': '摄像机受控代理可用',
  'net.ip': '本机 IP 地址（空＝由 DHCP 分配）',
  'net.mask': '子网掩码（空＝由 DHCP 分配）',
  'net.gateway': '网关 IP（空＝由 DHCP 分配）',
  'net.server.wired.host': '有线平台服务器地址',
  'net.server.wired.port': '有线平台服务器端口',
  'net.server.wireless.host': '无线平台服务器地址',
  'net.server.wireless.port': '无线平台服务器端口',
  'net.server.upgrade.host': '升级服务器地址',
  'net.server.upgrade.port': '升级服务器端口',
  'net.server.log.host': '日志上报服务器地址',
  'net.server.log.port': '日志上报服务器端口',
  'probe.target1': '主网检测 IP 1',
  'probe.target2': '主网检测 IP 2',
  'probe.interval_ms': '探测周期（毫秒）',
  'probe.timeout_ms': '单次探测超时（毫秒，必须 < 探测周期）',
  'probe.max_fail': '连续失败次数上限',
  'camera.search': '搜索模式（off/onvif/scan）',
  'camera.detect.mode': '摄像机检测方式：1=PING 2=端口扫描 3=RTSP',
  'camera.detect.rtt_ms': 'PING 方式延时判定阈值（毫秒）',
  'camera.snmp.enable': '摄像机 SNMP 采集开关',
  'camera.snmp.community': '摄像机 SNMP 团体字（读团体）',
  'camera.snmp.timeout_ms': '摄像机 SNMP 单次事务超时（毫秒）',
  'camera.snmp.retries': '摄像机 SNMP 重试次数（0=只发一次）',
  'camera.rec.hold_ms': '断网开录保持（毫秒）',
  'camera.rec.debounce_ms': '复网停录去抖（毫秒）',
  'switch.ip': '交换机 IP', 'switch.vendor': '交换机品牌', 'switch.model': '交换机型号',
  'switch.snmp.enable': '交换机 SNMP 采集开关',
  'switch.snmp.port': '交换机 SNMP 端口',
  'switch.snmp.community': '交换机 SNMP 团体字（读团体）',
  'switch.snmp.timeout_ms': '交换机 SNMP 单次事务超时（毫秒）',
  'switch.snmp.retries': '交换机 SNMP 重试次数（0=只发一次）',
  'elec.volt_high': '电压上限（V）', 'elec.volt_low': '电压下限（V）',
  'elec.over_current': '过流阈值（A）', 'elec.leak_current': '漏电流阈值（mA）',
  'elec.power': '功率阈值（W）',
  'netthr.timeout_ms': '网络超时时间（毫秒）',
  'netthr.loss_count': '丢包次数上限',
  'netthr.reboot_max': '自愈重启次数上限（0 = 禁用自愈）',
  'netthr.reboot_interval_s': '重启时间（秒，两次重启之间的最短间隔）',
  'sensor.tilt': '倾斜度（度）', 'sensor.temp_high': '温度上限（℃）',
  'sensor.temp_low': '温度下限（℃）', 'sensor.humi_high': '湿度上限（%）',
  'sensor.humi_low': '湿度下限（%）',
};
for (const i of CH_LIST) {
  const n = i + 1;
  Object.assign(descs, {
    [`camera.ch.${i}.vendor`]: `第 ${n} 路厂商`, [`camera.ch.${i}.ip`]: `第 ${n} 路 IP`,
    [`camera.ch.${i}.mac`]: `第 ${n} 路 MAC`, [`camera.ch.${i}.user`]: `第 ${n} 路用户名`,
    [`camera.ch.${i}.pass`]: `第 ${n} 路口令（SM4 密文）`, [`camera.ch.${i}.record`]: `第 ${n} 路是否录像`,
    [`camera.ch.${i}.stream`]: `第 ${n} 路码流类型（main/sub）`, [`camera.ch.${i}.url`]: `第 ${n} 路拉流地址`,
  });
}
for (const [k, d] of Object.entries({
  sysDescr: 'SNMP 槽位 sysDescr＝系统描述（string）',
  sysUpTime: 'SNMP 槽位 sysUpTime＝运行时长（TimeTicks/counter）',
  sysName: 'SNMP 槽位 sysName＝设备名称（string）',
  deviceType: 'SNMP 槽位 deviceType＝设备型号/类型（string）',
  softVersion: 'SNMP 槽位 softVersion＝软件版本（string）',
  serialNumber: 'SNMP 槽位 serialNumber＝序列号（string）',
  cpuPercent: 'SNMP 槽位 cpuPercent＝CPU 使用率（integer, %）',
  memUsed: 'SNMP 槽位 memUsed＝内存已用（gauge）',
  devStatus: 'SNMP 槽位 devStatus＝设备状态（integer）',
  ifDescr: 'SNMP 槽位 ifDescr＝端口描述（表列）',
  ifSpeed: 'SNMP 槽位 ifSpeed＝端口速率（表列）',
  ifOperStatus: 'SNMP 槽位 ifOperStatus＝端口物理态（表列）',
  ifInOctets: 'SNMP 槽位 ifInOctets＝端口入字节（表列）',
  ifOutOctets: 'SNMP 槽位 ifOutOctets＝端口出字节（表列）',
})) descs[`camera.snmp.oid.${k}`] = d;

let cfgVer = 42;

/* ==================== 路由 ==================== */
async function api(req, res, url) {
  const send = (code, j) => {
    const b = JSON.stringify(j);
    res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
    res.end(b);
  };
  const readBody = () => new Promise((r) => {
    let s = ''; req.on('data', (c) => (s += c)); req.on('end', () => r(s));
  });

  const p = url.pathname, m = req.method;
  /* 会话：只有 /api/login 放行，其余都要登录 Cookie（与真机 401 行为一致） */
  const authed = (req.headers.cookie || '').includes('ivsbox_mock_auth=1');
  if (p === '/api/login' && m === 'POST') {
    const b = JSON.parse((await readBody()) || '{}');
    if (b.user === 'admin' && b.pass === '88888888') {
      res.setHeader('Set-Cookie', 'ivsbox_mock_auth=1; Path=/');
      return send(200, { ok: true, d: { token: 'mock-token-0001', must_change: false } });
    }
    return send(200, { ok: false, msg: 'bad password' });
  }
  if (!authed) return send(401, { ok: false, msg: 'unauthorized', e: -1 });
  if (p === '/api/logout') { res.setHeader('Set-Cookie', 'ivsbox_mock_auth=1; Path=/; Max-Age=0'); return send(200, { ok: true }); }
  if (p === '/api/passwd') return send(200, { ok: true });
  if (p === '/api/overview') return send(200, ok(overview()));
  if (p === '/api/gps') return send(200, ok(gps()));
  if (p === '/api/board') return send(200, ok(board()));
  if (p === '/api/devices') return send(200, ok(devices()));
  if (p === '/api/camera/search' || p === '/api/snmp/run' ||
      p === '/api/system/time' || p === '/api/system/reboot' ||
      p === '/api/board/relay' || p === '/api/device/identity') {
    await readBody();
    return send(200, { ok: true, d: {} });
  }
  if (p === '/api/snmp') {
    return send(200, ok(snmp(url.searchParams.get('which') || 'cam')));
  }
  if (p === '/api/config') {
    const defaults = url.searchParams.get('defaults');
    if (defaults) {
      const items = {};
      for (const cls of Object.keys(configItems))
        for (const [k, v] of Object.entries(configItems[cls]))
          items[k] = { t: typeof v === 'boolean' ? 'bool' : typeof v === 'number' ? 'int' : 'str', v, d: descs[k] || '' };
      return send(200, ok({ version: cfgVer, items }));
    }
    const cls = url.searchParams.get('cls') || '';
    if (!configItems[cls]) return send(200, fail('bad class'));
    const items = {};
    for (const [k, v] of Object.entries(configItems[cls]))
      items[k] = typeof v === 'boolean' ? v : v;   /* 前端按值类型渲染 */
    return send(200, ok({ version: cfgVer, items }));
  }
  if (p === '/api/config/apply' && m === 'POST') {
    await readBody();
    cfgVer += 1;
    return send(200, { ok: true, d: { applied: 1, version: cfgVer } });
  }
  return send(404, { ok: false, msg: 'no such api', e: -1 });
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://127.0.0.1:${PORT}`);
  try {
    if (url.pathname.startsWith('/api/')) return await api(req, res, url);
    let file = url.pathname === '/' ? 'index.html'
      : url.pathname === '/prototype' ? 'prototype.html'
      : url.pathname.slice(1);
    file = normalize(file).replace(/^([/\\]|\.\.)+/, '');
    if (file.includes('..')) { res.writeHead(403); return res.end(); }
    /* prototype.html 在本目录（web_ui_redesign/），其余取真实前端 static/ */
    const base = file === 'prototype.html' ? HERE : STATIC_DIR;
    const body = await readFile(join(base, file));
    res.writeHead(200, { 'Content-Type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(body);
  } catch (e) {
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Not Found: ' + url.pathname);
  }
});

server.listen(PORT, () => {
  console.log(`mock server on http://127.0.0.1:${PORT}/  (admin / 88888888)`);
});
