/*
 * 前端行为测试：设备名称（`sys.device.name`）在总览页的配置入口
 *
 * ============================ 为什么要有这个 ============================
 * 这条改动一半在浏览器里（web/static/app.js 的总览页），`make test` / `make itest`
 * / `make arm` 全都碰不到它，`node --check` 也只管语法。本用例用 jsdom 把**真实的**
 * index.html + app.js 跑起来，断言：
 *   - 总览页「设备信息」里出现「设备名称」输入框与「保存名称」按钮；
 *   - 未命名时只读行显示「未命名」而不是空白（"空"和"没读到"要能分开）；
 *   - 点保存只发 **1 个键** `sys.device.name`，且版本号是**现取**的
 *     （本页没有 configCard 帮忙缓存版本号，拿旧值提交必撞 e=-3）；
 *   - 保存成功后回读、输入框跟着更新；清空后回到「未命名」。
 *
 * 后端那一半（配置落盘、overview.device.name、重启后仍在）由 VM 上的真 HTTP
 * 端到端取证，见 docs/修改记录.md 2026-10-22 条目。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_device_name.mjs
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

/* ---------------- fetch 桩：模拟后端三件事 ---------------- */
let name = '';        /* sys.device.name 的当前值 */
let ver = 3;          /* 故意不从 0 开始：能证明"版本号是现取的" */
const applyCalls = [];
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: {
                version: 'test',
                device: {
                    version: 'test', model: 'FN_RZTY_PY', soc: 'sun8iw20p1',
                    sn: '1', sn_source: 'default', id_mac: null, eth0_mac: null,
                    hostname: 'ivsbox', kernel: '5.4.61-rt37', machine: 'armv7l',
                    wall_sec: 1761000000,
                    /* 后端口径：没起名给 null，不是空串 */
                    name: name === '' ? null : name
                },
                uptime_ms: 123456
            }
        }));
    }
    if (u.indexOf('/api/config?defaults=1') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: { items: { 'sys.device.name': { v: '', d: '设备名称（本机名称，如「3 号换热站」）' } } }
        }));
    }
    if (u.indexOf('/api/config?cls=sys') === 0) {
        return Promise.resolve(rsp({
            ok: true, d: { version: ver, items: { 'sys.device.name': name } }
        }));
    }
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        if (body.ver !== ver)   /* 真实后端：版本不符即整条拒（e=-3） */
            return Promise.resolve(rsp({ ok: false, e: -3, msg: 'version conflict' }));
        Object.keys(body.items).forEach((k) => {
            if (k === 'sys.device.name')
                name = body.items[k];
        });
        ver += 1;
        return Promise.resolve(rsp({ ok: true, d: { version: ver, applied: Object.keys(body.items).length } }));
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

const main = async () => {
    await sleep(50);

    console.log('v01 进入总览页');
    const nav = byText(doc.getElementById('navList'), 'button', '总览') ||
        byText(doc.getElementById('navList'), 'a', '总览');

    chk(nav !== null, 'v01a: 导航里有「总览」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(80);

    const content = doc.getElementById('content');
    const saveBtn = byText(content, 'button', '保存名称');
    const inputs = Array.from(content.querySelectorAll('input[type="text"]'));
    const nameInp = inputs.find((i) => /换热站/.test(i.placeholder || ''));

    chk(saveBtn !== null, 'v01b: 总览页出现「保存名称」按钮');
    chk(nameInp !== undefined, 'v01c: 出现设备名称输入框（placeholder 带示例）');
    if (saveBtn === null || nameInp === undefined)
        return;

    console.log('v02 出厂未命名');
    chk(nameInp.value === '', 'v02a: 未命名时输入框为空');
    const cardText = content.textContent;
    chk(cardText.indexOf('未命名') >= 0, 'v02b: 只读行显示「未命名」而不是空白/undefined');

    console.log('v03 填名称并保存');
    nameInp.value = '  3 号换热站  ';      /* 故意带空白：前端要去掉 */
    nameInp.dispatchEvent(new window.Event('input'));
    saveBtn.click();
    await sleep(120);

    chk(applyCalls.length === 1, 'v03a: 只发一次 config.apply');
    const body = applyCalls[0] || { items: {}};

    chk(Object.keys(body.items).length === 1, 'v03b: 只发 1 个键');
    chk(body.items['sys.device.name'] === '3 号换热站',
        'v03c: 键名与值正确（已去首尾空白），实得 ' + JSON.stringify(body.items));
    chk(body.ver === 3, 'v03d: 版本号是现取的（=3，不是缓存的 0）');

    console.log('v04 保存后回读');
    chk(nameInp.value === '3 号换热站', 'v04a: 输入框回填读回来的值，实得「' + nameInp.value + '」');
    chk(content.textContent.indexOf('未命名') < 0, 'v04b: 「未命名」消失');

    console.log('v05 清空名称');
    nameInp.value = '';
    nameInp.dispatchEvent(new window.Event('input'));
    saveBtn.click();
    await sleep(120);
    chk(applyCalls.length === 2, 'v05a: 清空也发 config.apply');
    chk(applyCalls[1] !== undefined && applyCalls[1].items['sys.device.name'] === '',
        'v05b: 提交空串（把名字清掉）');
    chk(applyCalls[1] !== undefined && applyCalls[1].ver === 4,
        'v05c: 版本号跟上一次返回值（ver=4）');
    chk(content.textContent.indexOf('未命名') >= 0, 'v05d: 清空后回到「未命名」');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_device_name passed (15 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
