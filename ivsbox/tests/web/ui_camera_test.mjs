/*
 * 前端行为测试：摄像机页三件事（检测方式下拉 / 单路「测」/ 查摄像机 SNMP）
 *
 * ============================ 为什么要有这个 ============================
 * 这一批改动全在浏览器里（web/static/app.js），`make test`、`make itest`、
 * `make arm` **全都碰不到它** —— `node --check` 只证明语法。
 *
 * 三条最容易出错、而且错了很难看出来的接线，正好都在这张卡上：
 *   ① `ch` 的**基号**：后端 `camera.test` 收 1..6（与页面「摄像机N」同序），
 *      而 `camera.ch.N.ip` 的 N 是 0 基。写错就是"点 3 号在测 4 号"，
 *      界面上一切正常，只有结论不对。
 *   ② `camera.detect.mode` 必须发**数字**而不是字符串：配置模块按默认值表的
 *      类型收口（D_INT ⇒ int），发 "2" 会被判类型不符，apply 默默不生效。
 *   ③ `IV_ENOENT = -10`（不是 -2，-2 是 IV_EINVAL）：因为 ENOENT 是"这一路
 *      没配 IP"这个最常见的现场情形，写错码号就永远走不到那句提示。
 *
 * 做法：jsdom 跑**真实的** index.html + app.js，走
 *   进「摄像机」页 → 改检测方式并保存 → 点第 3 路的「测」→ 查摄像机 SNMP
 * 断言请求体、提示文案、以及"重测后才回读"的时序。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_camera_test.mjs
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
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

/* ---------------- 桩状态 ---------------- */
let ver = 0;
let overviewLoads = 0;        /* /api/overview 被读了几次（验证"重测后才回读"） */
let camMode = 3;              /* camdet 当前生效的检测方式（后端真值） */
let testReply = { ok: true, d: { queued: true, ch: 3, mode: 3, mode_name: 'rtsp', expect_ms: 20000 } };
const applyCalls = [];
const testCalls = [];
const snmpRunCalls = [];
const configItems = {
    'camera.detect.mode': 3,
    'camera.detect.rtt_ms': 200
};

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0) {
        overviewLoads++;
        /* 六路 CNS：故意造出 0/1/2/4/5 五态，验证名称映射与配色分支 */
        return Promise.resolve(rsp({
            ok: true, d: { version: 'test', cameras: { cns: [1, 2, 0, 4, 5, 1], mode: camMode } }
        }));
    }
    if (u.indexOf('/api/config?defaults=1') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    if (u.indexOf('/api/config?cls=camera') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: configItems } }));
    if (u.indexOf('/api/config?cls=') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        Object.keys(body.items).forEach((k) => {
            configItems[k] = body.items[k];
            if (k === 'camera.detect.mode')
                camMode = body.items[k];   /* 后端热生效：overview 立刻反映新方式 */
        });
        ver += 1;
        return Promise.resolve(rsp({ ok: true, d: { version: ver, applied: Object.keys(body.items).length } }));
    }
    if (u.indexOf('/api/camera/test') === 0) {
        testCalls.push(JSON.parse(o.body));
        return Promise.resolve(rsp(testReply));
    }
    if (u.indexOf('/api/snmp/run') === 0) {
        snmpRunCalls.push(JSON.parse(o.body));
        return Promise.resolve(rsp({ ok: true, d: { accepted: true } }));
    }
    if (u.indexOf('/api/snmp?which=') === 0) {
        return Promise.resolve(rsp({
            ok: true, d: {
                state: 2,
                slots: [{ name: 'sysDescr', oid: '.1.3.6.1.2.1.1.1.0', n_rows: 1, rows: [{ str: 'HIKVISION IPC' }] }]
            }
        }));
    }
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
const toastText = () => {
    const t = doc.getElementById('toast');

    return t ? t.textContent : '';
};

const main = async () => {
    await sleep(50);

    /* 进摄像机页 */
    const nav = byText(doc.getElementById('navList'), 'button', '摄像机') ||
        byText(doc.getElementById('navList'), 'a', '摄像机');

    chk(nav !== null, 'v00a: 导航里有「摄像机」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(80);

    const content = doc.getElementById('content');

    console.log('v01 检测方式下拉');
    const modeSel = content.querySelector('select');
    const saveBtn = byText(content, 'button', '保存并立即生效');

    chk(byText(content, 'h3', '检测方式') !== null, 'v01a: 出现「检测方式」卡');
    chk(modeSel !== null, 'v01b: 出现下拉框');
    chk(saveBtn !== null, 'v01c: 出现「保存并立即生效」按钮');
    if (modeSel === null || saveBtn === null)
        return;

    chk(modeSel.options.length === 3, 'v01d: 三个选项，实得 ' + modeSel.options.length);
    const optText = [...modeSel.options].map((o) => o.textContent).join('/');

    chk(optText === 'PING方式/端口扫描/RTSP拉流测试',
        'v01e: 选项文案与参考 web 一致，实得「' + optText + '」');
    chk(modeSel.value === '3',
        'v01f: 默认值跟随 camdet 真实状态（mode=3 ⇒ RTSP），实得 ' + modeSel.value);

    console.log('v02 保存检测方式 → config.apply');
    modeSel.value = '2';                 /* 切到端口扫描 */
    saveBtn.click();
    await sleep(80);

    chk(applyCalls.length === 1, 'v02a: 只发一次 config.apply');
    const a1 = applyCalls[0] || { items: {} };

    chk(a1.ver === 0, 'v02b: 带当前版本号 ver=0');
    chk(Object.keys(a1.items).length === 1, 'v02c: 只发 1 个键');
    chk(typeof a1.items['camera.detect.mode'] === 'number',
        'v02d: 值是**数字**而不是字符串（配置模块按 D_INT 收口），实得 ' +
        typeof a1.items['camera.detect.mode']);
    chk(a1.items['camera.detect.mode'] === 2, 'v02e: 值为 2（端口扫描）');
    chk(/检测方式已切到/.test(toastText()), 'v02f: 提示「检测方式已切到…」，实得「' + toastText() + '」');

    /* 保存后要重读一次 overview：下拉回填的是后端真值，不是刚点的那一项 */
    modeSel.value = '1';
    saveBtn.click();
    await sleep(80);
    chk(applyCalls.length === 2 && applyCalls[1].ver === 1,
        'v02g: 版本号跟随上一次返回值前移（ver=1）');

    console.log('v03 单路「测」按钮');
    const testBtns = [...content.querySelectorAll('button')].filter((b) => b.textContent.trim() === '测');

    chk(testBtns.length === 6, 'v03a: 六路各一个「测」，实得 ' + testBtns.length);
    if (testBtns.length !== 6)
        return;

    const loadsBefore = overviewLoads;

    testBtns[2].click();                 /* 摄像机3 */
    await sleep(80);

    chk(testCalls.length === 1, 'v03b: 只发一次 camera.test');
    chk(testCalls[0] && testCalls[0].ch === 3,
        'v03c: ch 是**1 基**：点第 3 个按钮发 ch=3（后端再翻成下标 2），实得 ' +
        JSON.stringify(testCalls[0]));
    chk(/已在测摄像机3/.test(toastText()), 'v03d: 提示点名了第 3 路，实得「' + toastText() + '」');
    chk(/20 秒/.test(toastText()),
        'v03e: 用后端给的 expect_ms 换算成"约 20 秒"（PING/RTSP 时长不同），实得「' + toastText() + '」');
    chk(overviewLoads === loadsBefore, 'v03f: 点完不立刻回读（要等 expect_ms）');

    console.log('v04 ENOENT(-10)：这一路没配 IP');
    testReply = { ok: false, e: -10, msg: 'this channel has no IP configured' };
    testBtns[4].click();
    await sleep(80);
    chk(/还没配 IP/.test(toastText()),
        'v04a: 走 IV_ENOENT 分支给准话（-10，不是 -2），实得「' + toastText() + '」');
    chk(overviewLoads === loadsBefore, 'v04b: 失败不回读');

    console.log('v05 失败后按钮要能再点（冷却标志只能在失败分支解开）');
    chk(testBtns[4].disabled === false, 'v05a: 失败后按钮已恢复可用');

    console.log('v06 expect_ms 到期后自动回读 overview');
    testReply = { ok: true, d: { queued: true, ch: 6, mode: 1, mode_name: 'ping', expect_ms: 60 } };
    testBtns[5].click();
    await sleep(200);
    chk(overviewLoads > loadsBefore,
        'v06a: 到期后回读了一次（' + loadsBefore + ' → ' + overviewLoads + '）');

    console.log('v07 查摄像机（SNMP）');
    chk(byText(content, 'h3', '查摄像机（SNMP）') !== null, 'v07a: 摄像机页出现 SNMP 卡');
    const snmpBtn = byText(content, 'button', '发起采集');

    chk(snmpBtn !== null, 'v07b: 有「发起采集」按钮');
    chk(/sysDescr/.test(content.textContent), 'v07c: 结果表渲染出槽位名');
    if (snmpBtn === null)
        return;
    snmpBtn.click();
    await sleep(80);
    chk(snmpRunCalls.length === 1, 'v07d: 发一次 snmp.run');
    chk(snmpRunCalls[0] && snmpRunCalls[0].which === 'cam',
        'v07e: which=cam（第 1 路摄像机），实得 ' + JSON.stringify(snmpRunCalls[0]));

    console.log('v08 交换机页不再重复挂摄像机采集入口');
    const navSw = byText(doc.getElementById('navList'), 'button', '交换机');

    if (navSw !== null) {
        navSw.click();
        await sleep(80);
        const swContent = doc.getElementById('content');

        chk(byText(swContent, 'button', '采集摄像机') === null,
            'v08a: 交换机页没有「采集摄像机」（已归摄像机页）');
        chk(byText(swContent, 'button', '发起采集') !== null, 'v08b: 交换机页仍有自己的采集入口');
    } else {
        chk(false, 'v08a: 导航里找不到「交换机」');
    }
};

main().then(() => {
    console.log(fail === 0 ? 'ui_camera_test passed (22 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
