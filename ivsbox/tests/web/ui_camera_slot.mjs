/*
 * 前端行为测试：摄像机通道建档（「新增摄像机通道」按钮）
 *
 * ============================ 为什么要有这个 ============================
 * 这条改动整个在浏览器里（web/static/app.js + index.html），`make test`、
 * `make itest`、`make arm` **全都碰不到它** —— 只做 `node --check` 语法检查
 * 等于没验证。写好之后第一次跑就抓到一个真缺陷（D-16：提示条 #toast 挂在
 * #content 里，登录时被 go() 清空，之后每次 toast() 抛 TypeError，把调用方
 * 后面的"版本号更新 + 表单重载"一起带走 ⇒ 保存后必须手动刷新才能再保存）。
 *
 * 做法：用 jsdom 把**真实的** index.html + app.js 跑起来，走一遍
 *   登录成功 → 点导航进「摄像机」→ 选通道 3 → 填 IP → 点「建立该路通道」
 * 然后断言：
 *   - 发给 /api/config/apply 的 body 是 `{ver, items:{camera.ch.2.ip}}`、
 *     只发 1 个键（其余 7 项靠后端 set_post_update→fill_missing_defaults 补齐，
 *     这一点由 VM 上的 HTTP 端到端取证独立确认：1 键 ⇒ 8 键）；
 *   - 空 IP 不发请求；
 *   - 建槽后下拉框文案变成「已配置：<ip>」，IP 框跟着回填，版本号只在成功后前移。
 *
 * 后端的"整槽补齐"在这里用桩模拟（apply 后补齐同槽 8 键），所以本用例只证明
 * **前端**接线正确；后端那一半见 `docs/修改记录.md` 2026-10-21 的 D-06 取证输出。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_camera_slot.mjs
 * 例：
 *   NODE_PATH=/path/to/node_modules node tests/web/ui_camera_slot.mjs
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
    /* 用仓库自己的解析路径找 jsdom；找不到就 SKIP（不把 node 生态变成构建依赖）。 */
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

/* ---------------- fetch 桩：只回答本用例用到的 4 个接口 ---------------- */
const CAM_DEFAULTS = { mac: '', pass: '', record: false, stream: '', url: '', user: '', vendor: '' };
let items = {};              /* camera 分类当前的键值 */
let ver = 0;
const applyCalls = [];

const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 'test', cameras: { cns: [] } } }));
    if (u.indexOf('/api/config?defaults=1') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    if (u.indexOf('/api/config?cls=camera') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: items } }));
    if (u.indexOf('/api/snmp') === 0)
        return Promise.resolve(rsp({ ok: true, d: { state: 2, slots: [] } }));
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        /* 模拟真实后端 set_post_update→fill_missing_defaults：写进去的键生效，
         * 同一槽位的其余键被整套补齐。 */
        Object.keys(body.items).forEach((k) => {
            items[k] = body.items[k];
            const m = /^camera\.ch\.(\d+)\./.exec(k);

            if (m === null)
                return;
            const pre = 'camera.ch.' + m[1] + '.';

            Object.keys(CAM_DEFAULTS).forEach((f) => {
                if (items[pre + f] === undefined)
                    items[pre + f] = CAM_DEFAULTS[f];
            });
        });
        ver += 1;
        return Promise.resolve(rsp({ ok: true, d: { version: ver, applied: Object.keys(body.items).length } }));
    }
    /* 兜底：没单独写桩的接口一律回空 d。**不要**指望这条兜底能顶替真桩——
     * R-07 就是这么来的：`/api/snmp` 没有桩，兜底回 `d:{}`，页面里
     * `r.d.slots.length` 抛 TypeError，异常把整页后续渲染带走，而用例因为
     * 没人断言那块内容照样"全绿"。所以这里打一行警告，露出来。 */
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

const main = async () => {
    await sleep(50);

    console.log('u01 进入摄像机页');
    const nav = byText(doc.getElementById('navList'), 'button', '摄像机') ||
        byText(doc.getElementById('navList'), 'a', '摄像机');

    chk(nav !== null, 'u01a: 导航里有「摄像机」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(50);

    const content = doc.getElementById('content');
    const btn = byText(content, 'button', '建立该路通道');
    /* 页面上还有别的 select/input（检测方式、RTSP 拉流、SNMP 槽位…），
     * 按"第几个元素"取会随卡片增删而错位 —— 用控件自身的 title/placeholder 定位
     * （这两个属性在 cameraSlotCard() 里是写死的，见 app.js）。 */
    const sel = content.querySelector('select[title="通道序号"]');
    const ipInp = content.querySelector('input[placeholder^="摄像机 IP"]');

    chk(btn !== null, 'u01b: 摄像机页出现「建立该路通道」按钮');
    chk(sel !== null, 'u01c: 出现通道序号下拉');
    if (btn === null || sel === null)
        return;

    console.log('u02 出厂状态（没有任何 camera.ch.* 键）');
    chk(sel.options.length === 8, 'u02a: 下拉给出 8 个候补序号（与录像 8 路对齐）');
    chk(/未配置/.test(sel.options[0].textContent), 'u02b: 未建槽时标「未配置」');

    console.log('u03 选通道 3、填 IP、点建立');
    sel.value = '2';
    sel.dispatchEvent(new window.Event('change'));
    ipInp.value = '  10.0.0.9  ';          /* 故意带空白：前端要去掉 */
    btn.click();
    await sleep(80);

    chk(applyCalls.length === 1, 'u03a: 只发一次 config.apply');
    const body = applyCalls[0] || { items: {}};

    chk(body.ver === 0, 'u03b: 带上当前版本号 ver=0（缺 ver 后端会拒）');
    chk(Object.keys(body.items).length === 1, 'u03c: 只发 1 个键（其余交后端补齐）');
    chk(body.items['camera.ch.2.ip'] === '10.0.0.9',
        'u03d: 键名 camera.ch.2.ip、值已去首尾空白，实得 ' + JSON.stringify(body.items));

    console.log('u04 建槽后重载：下拉文案与 IP 回填（D-16 就在这里炸过）');
    chk(sel.options.length === 8, 'u04a: 重载后仍是 8 个序号');
    chk(/已配置：10\.0\.0\.9/.test(sel.options[2].textContent),
        'u04b: 通道 3 标「已配置：10.0.0.9」，实得「' + sel.options[2].textContent + '」');
    chk(/未配置/.test(sel.options[0].textContent), 'u04c: 通道 1 仍是「未配置」');
    chk(ipInp.value === '10.0.0.9', 'u04d: 选中项变了 IP 框跟着回填');

    console.log('u05 空 IP 必须被挡在前端（不发请求）');
    sel.value = '4';
    sel.dispatchEvent(new window.Event('change'));
    chk(ipInp.value === '', 'u05a: 切到未配置的通道 5，IP 框清空');
    btn.click();
    await sleep(50);
    chk(applyCalls.length === 1, 'u05b: 空 IP 不发 config.apply');

    console.log('u06 覆盖已有通道（改 IP）');
    sel.value = '2';
    sel.dispatchEvent(new window.Event('change'));
    ipInp.value = '10.0.0.10';
    btn.click();
    await sleep(80);
    chk(applyCalls.length === 2, 'u06a: 覆盖时也发 config.apply');
    chk(applyCalls[1] !== undefined && applyCalls[1].ver === 1,
        'u06b: 版本号已跟上 u03 的返回值（ver=1）');
    chk(applyCalls[1] !== undefined && applyCalls[1].items['camera.ch.2.ip'] === '10.0.0.10',
        'u06c: 新 IP 已发出');

    console.log('u07 「查摄像机（SNMP）」卡片：接口回空 slots 时不得抛异常（R-07）');
    /* 原先这里 `r.d.slots.length` 直接取值：桩缺 /api/snmp 时兜底回 `d:{}`，
     * 于是抛 TypeError，整页后续渲染被带走，而用例因为没人断言这块内容照样全绿。 */
    chk(byText(content, 'p', '尚无采集结果') !== null,
        'u07a: SNMP 卡片渲染「尚无采集结果」（旧代码此处 TypeError）');
    chk(byText(content, 'p', '读取中…') === null, 'u07b: 卡片已走完「读取中…」');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_camera_slot passed (21 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
