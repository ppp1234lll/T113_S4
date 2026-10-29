/*
 * 前端行为测试：摄像机页「品牌按设备发现结果自动匹配」（2026-10-29）
 *
 * ============================ 为什么要有这个 ============================
 * 用户现场提问："搜索完摄像机、也解析出品牌了，配好 IP 为什么品牌不自动匹配？"
 * 根因有两层，都在这一条链路上：
 *   1. 后端 `iv_device` 报的品牌名（hikvision / dahua / unv / onvif）与配置键
 *      `camera.ch.N.vendor` 的取值域（hik / dahua / uniview / huawei / jovision）
 *      **是两套命名**，直接 `select.value = 'hikvision'` 赋不上值 ⇒ 下拉留空
 *      ⇒ 界面回落"（默认：海康）"，看起来就是"没匹配"；
 *   2. 摄像机页从来没有把发现结果按 IP 喂给下拉的代码 —— `d.brand` 只画在
 *      「设备档案」表格里。
 *
 * 这条路径 `make test` / `itest` / `arm` **全都碰不到**（纯浏览器逻辑），
 * 所以必须有 jsdom 用例；只做 `node --check` 等于没验证。
 *
 * 断言四件事：
 *   v01 具体厂商（hikvision）⇒ 自动选 `hik`，地址按海康重算；
 *   v02 具体厂商（dahua）    ⇒ 自动选 `dahua`，地址变成 /cam/realmonitor；
 *   v03 泛 ONVIF（onvif）    ⇒ **不填**、只提示（填了只能猜，一半概率拉不通）；
 *   v04 用户手选过品牌       ⇒ 改 IP 也不再覆盖（"我明明选了华为"）；
 *   v05 没发现过的 IP        ⇒ 保持空、不弹提示（不能打扰）；
 *   v06 `/api/devices` 只拉一次（8 路通道不该打 8 次）。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_camera_vendor.mjs
 */
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const webDir = path.resolve(here, '../../web/static');
const appPath = path.join(webDir, 'app.js');
const htmlPath = path.join(webDir, 'index.html');

let JSDOM;
try {
    const req = createRequire(path.join(here, 'noop.js'));
    ({ JSDOM } = req('jsdom'));
} catch (e) {
    console.log('SKIP: jsdom not installed (npm i jsdom, or set NODE_PATH); ' + e.message);
    process.exit(0);
}

const src = fs.readFileSync(appPath, 'utf8');
const html = fs.readFileSync(htmlPath, 'utf8');

const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://127.0.0.1/' });
const { window } = dom;
const doc = window.document;

let fail = 0;
function chk(cond, what) {
    if (cond) {
        console.log('  PASS ' + what);
    } else {
        console.log('  FAIL ' + what);
        fail++;
    }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---------------- 桩数据 ---------------- */
/* 三路通道：都是"IP 已填、品牌还没选"的现场形态（这正是用户截图那一刻的状态）。 */
const items = {
    'camera.ch.0.ip': '192.168.2.18',
    'camera.ch.0.vendor': '',
    'camera.ch.0.stream': '',
    'camera.ch.0.url': '',
    'camera.ch.1.ip': '192.168.1.106',
    'camera.ch.1.vendor': '',
    'camera.ch.1.stream': '',
    'camera.ch.1.url': '',
    'camera.ch.2.ip': '192.168.1.108',
    'camera.ch.2.vendor': '',
    'camera.ch.2.stream': '',
    'camera.ch.2.url': ''
};

/* 设备发现结果，取自板端 `device.list` 的真实返回（2026-10-29 实测）。
 * `.108` 当时报的是 onvif —— 后端品牌判定已修，但前端仍必须对 onvif 保守处理。 */
const DEVICES = [
    { brand: 'hikvision', ip: '192.168.2.18', mac: 'd4:e8:53:5c:0e:46',
      model: 'DS-IPC-T12HV3-IA', serial: 'x', port: 80, found_by: 'hik-udp' },
    { brand: 'dahua', ip: '192.168.1.106', mac: '40:7a:a4:86:95:62',
      model: 'DH-IPC-HFW', serial: '', port: 80, found_by: 'dahua-mcast' },
    { brand: 'onvif', ip: '192.168.1.108', mac: 'e0:2e:fe:de:92:b5',
      model: 'Dahua', serial: '', port: 80, found_by: 'dahua-mcast' }
];

let devicesCalls = 0;

const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 'test', cameras: { cns: [] } } }));
    if (u.indexOf('/api/config?defaults=1') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 0, items: {} } }));
    if (u.indexOf('/api/config?cls=camera') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 0, items: items } }));
    if (u.indexOf('/api/snmp') === 0)
        return Promise.resolve(rsp({ ok: true, d: { state: 2, slots: [] } }));
    if (u.indexOf('/api/devices') === 0) {
        devicesCalls++;
        return Promise.resolve(rsp({ ok: true, d: { count: DEVICES.length, items: DEVICES } }));
    }
    if (u.indexOf('/api/config/apply') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 1, applied: 0 } }));
    console.log('  WARN unstubbed api: ' + u);
    return Promise.resolve(rsp({ ok: true, d: {} }));
};

window.eval(src);
if (doc.readyState === 'loading')
    doc.dispatchEvent(new window.Event('DOMContentLoaded'));

function byText(root, tag, text) {
    for (const e of root.querySelectorAll(tag)) {
        if (e.textContent.trim() === text)
            return e;
    }
    return null;
}

/* 按"第 N 路"取该通道区块（区块标题写死为 `第 N 路（camera.ch.X）`）。 */
function secOf(content, n) {
    const h = byText(content, 'h3', '第 ' + n + ' 路（camera.ch.' + (n - 1) + '）');
    return h === null ? null : h.parentElement;
}
const selOf = (sec, n, f) =>
    sec.querySelector('label[title="camera.ch.' + (n - 1) + '.' + f + '"] select');
const inpOf = (sec, n, f) =>
    sec.querySelector('label[title="camera.ch.' + (n - 1) + '.' + f + '"] input');

const main = async () => {
    await sleep(50);

    const nav = byText(doc.getElementById('navList'), 'button', '摄像机') ||
        byText(doc.getElementById('navList'), 'a', '摄像机');
    chk(nav !== null, 'v00a: 导航里有「摄像机」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(120);        /* 等 load() + 三路的 /api/devices 都回来 */

    const content = doc.getElementById('content');
    const s1 = secOf(content, 1);
    const s2 = secOf(content, 2);
    const s3 = secOf(content, 3);
    chk(s1 !== null && s2 !== null && s3 !== null, 'v00b: 三路通道区块都画出来了');
    if (s1 === null || s2 === null || s3 === null)
        return;

    console.log('v01 海康：发现结果 hikvision ⇒ 自动选 hik');
    const b1 = selOf(s1, 1, 'vendor');
    const u1 = inpOf(s1, 1, 'url');
    chk(b1 !== null && b1.value === 'hik',
        'v01a: 第 1 路品牌自动变成 hik，实得「' + (b1 ? b1.value : 'null') + '」');
    chk(u1 !== null && u1.value === 'rtsp://192.168.2.18:554/Streaming/Channels/101',
        'v01b: 拉流地址按海康重算，实得「' + (u1 ? u1.value : 'null') + '」');
    chk(b1 !== null && b1.className === 'dirty', 'v01c: 品牌标成待保存（dirty）');
    {
        const hint = s1.querySelector('p.hint');
        chk(hint !== null && hint.style.display !== 'none' &&
            /海康/.test(hint.textContent), 'v01d: 提示里说明了自动选了海康');
    }

    console.log('v02 大华：发现结果 dahua ⇒ 自动选 dahua，地址换成 realmonitor');
    const b2 = selOf(s2, 2, 'vendor');
    const u2 = inpOf(s2, 2, 'url');
    chk(b2 !== null && b2.value === 'dahua',
        'v02a: 第 2 路品牌自动变成 dahua，实得「' + (b2 ? b2.value : 'null') + '」');
    chk(u2 !== null && u2.value ===
        'rtsp://192.168.1.106:554/cam/realmonitor?channel=1&subtype=0',
        'v02b: 地址换成大华格式，实得「' + (u2 ? u2.value : 'null') + '」');

    console.log('v03 泛 ONVIF：只提示、不改下拉（填了也只能猜）');
    const b3 = selOf(s3, 3, 'vendor');
    const u3 = inpOf(s3, 3, 'url');
    chk(b3 !== null && b3.value === '',
        'v03a: 第 3 路品牌保持空（onvif 无对应项），实得「' + (b3 ? b3.value : 'null') + '」');
    chk(u3 !== null && u3.value === 'rtsp://192.168.1.108:554/Streaming/Channels/101',
        'v03b: 地址没被乱改（仍是默认海康的草稿）');
    {
        const hint = s3.querySelector('p.hint');
        chk(hint !== null && hint.style.display !== 'none' &&
            /判断不出厂商/.test(hint.textContent),
            'v03c: 明确提示「判断不出厂商，请手动选」，实得「' +
            (hint ? hint.textContent : 'null') + '」');
    }

    console.log('v04 用户手选过品牌 ⇒ 改 IP 也不再覆盖');
    b2.value = 'huawei';
    b2.dispatchEvent(new window.Event('change'));
    await sleep(10);
    const ip2 = inpOf(s2, 2, 'ip');
    ip2.value = '192.168.1.106';
    ip2.dispatchEvent(new window.Event('change'));
    await sleep(80);
    chk(b2.value === 'huawei', 'v04a: 手选的 huawei 被保住，实得「' + b2.value + '」');
    {
        const hint = s2.querySelector('p.hint');
        chk(hint === null || hint.style.display === 'none',
            'v04b: 手选后自动匹配的提示已撤掉');
    }

    console.log('v05 没发现过的设备 ⇒ 保持空、不打扰');
    const ip3 = inpOf(s3, 3, 'ip');
    ip3.value = '10.9.9.9';
    ip3.dispatchEvent(new window.Event('change'));
    await sleep(80);
    chk(b3.value === '', 'v05a: 未发现过的 IP 不会瞎填品牌');
    {
        const hint = s3.querySelector('p.hint');
        chk(hint === null || hint.style.display === 'none',
            'v05b: 也不弹提示（没消息就是好消息）');
    }

    console.log('v06 /api/devices 有缓存，8 路不该打 8 次');
    chk(devicesCalls <= 2, 'v06a: 三路通道只拉了 ' + devicesCalls + ' 次设备档案（≤2）');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_camera_vendor passed (16 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
