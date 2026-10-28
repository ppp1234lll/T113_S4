/*
 * 前端行为测试：媒体页「录像」卡（2026-10-28）
 *
 * ============================ 为什么要有这个 ============================
 * 这一批改动一半在浏览器里（`web/static/app.js` 的 pageMedia），`make test` /
 * `make itest` / `make arm` 全都碰不到它，`node --check` 只证明语法。
 * 而这一页最容易出的三件事，恰好都是"后端对了、界面上看不出来"：
 *
 *   ① **一句话结论必须分清三种态**：自动录像（主网断触发）/ 手动录像（不受主网管）/
 *      已人工停止（在压制自动开录）。三种都写成"正在录像"或"未录像"，现场就会
 *      把"按了停止不生效"和"手动录像停不下来"当成两个 bug（2026-10-28 板端都踩过）。
 *   ② **失败原因必须照抄后端**：后端给的是"摄像机端口未开放：192.168.1.64:554
 *      拒绝连接"这种能照着修的句子。前端一旦改写成"操作失败"，这一页就没有
 *      存在的意义了（用户 2026-10-28 明确要求：起不来要能看出是什么原因）。
 *   ③ **不能把 RTSP 地址露出去**：地址里带摄像机账密，后端只回布尔，前端也不许
 *      自己拼/回显（断言整页文本里没有 `rtsp://` 与 IP）。
 *
 * 做法：jsdom 跑**真实的** index.html + app.js，走
 *   进「媒体」页 → 点「立即录像」/「停止录像」→ 切换后端状态再回读。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_media_record.mjs
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

/* ---------------- fetch 桩：只实现 /api/media/record ---------------- */
/* 8 路的形态照后端 `record.page`：第 1 路正常在录；第 2 路配了地址但开关关着；
 * 第 3 路配了地址、开关开着、但被判失败（带原因）；其余未配置。 */
function makeChannels() {
    const ch = [];

    for (let i = 0; i < 8; i++)
        ch.push({ id: i, url: false, want: false, on: false, failed: false });
    ch[0] = { id: 0, url: true, want: true, on: true, failed: false };
    ch[1] = { id: 1, url: true, want: false, on: false, failed: false };
    ch[2] = { id: 2, url: true, want: true, on: false, failed: true,
              why: '摄像机端口未开放：192.168.1.64:554 拒绝连接（端口不对或 RTSP 服务未开）' };
    return ch;
}

let pageData = { recording: true, forced: false, held_off: false, count: 8, ch: makeChannels() };
let postRsp = { ok: true, d: { on: true, changed: true } };
const posts = [];
let gets = 0;
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/media/record') === 0) {
        if ((o.method || 'GET').toUpperCase() === 'POST') {
            posts.push({ method: o.method, body: JSON.parse(o.body) });
            return Promise.resolve(rsp(postRsp));
        }
        gets++;
        return Promise.resolve(rsp({ ok: true, d: pageData }));
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

    console.log('m01 进「媒体」页');
    const nav = byText(doc.getElementById('navList'), 'button', '媒体') ||
        byText(doc.getElementById('navList'), 'a', '媒体');

    chk(nav !== null, 'm01a: 导航里有「媒体」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(120);

    const content = doc.getElementById('content');
    const btnOn = byText(content, 'button', '立即录像');
    const btnOff = byText(content, 'button', '停止录像');

    chk(btnOn !== null, 'm01b: 出现「立即录像」按钮');
    chk(btnOff !== null, 'm01c: 出现「停止录像」按钮');
    if (btnOn === null || btnOff === null)
        return;
    chk(gets >= 1, 'm01d: 进页就读了一次 record.page');

    console.log('m02 自动录像态（主网断触发）');
    chk(content.textContent.indexOf('主网断开触发') >= 0,
        'm02a: 结论写明是自动（主网断开触发）');
    chk(content.textContent.indexOf('手动启动') < 0,
        'm02b: 自动态不许标成手动');

    console.log('m03 三态表格');
    {
        const rows = content.querySelectorAll('table tr');
        const text = content.textContent;

        chk(rows.length === 9, 'm03a: 表头 + 8 行通道，实得 ' + rows.length);
        chk(text.indexOf('录制中') >= 0, 'm03b: 在录的那一路标「录制中」');
        chk(text.indexOf('录像开关关闭') >= 0, 'm03c: 开关关掉的那一路说清是开关问题');
        chk(text.indexOf('未配置地址') >= 0, 'm03d: 没配地址的那一路说清是没配');
        chk(text.indexOf('无法启动') >= 0, 'm03e: 判定失败的那一路标「无法启动」');
        /* m03f 是这一页的存在理由：原因必须**原样**出现（不许被改写成"操作失败"） */
        chk(text.indexOf('摄像机端口未开放：192.168.1.64:554 拒绝连接') >= 0,
            'm03f: 失败原因原样显示（照这句话就能修）');
        /* m03g/m03h：地址含账密，页面不许露 */
        chk(text.indexOf('rtsp://') < 0, 'm03g: 整页不含 rtsp:// 地址');
        chk(text.indexOf('admin:') < 0, 'm03h: 整页不含账密片段');
    }

    console.log('m04 点「立即录像」');
    btnOn.click();
    await sleep(150);
    chk(posts.length === 1, 'm04a: 只发一次 POST');
    chk(posts[0] !== undefined && posts[0].method === 'POST', 'm04b: 用 POST（不是 GET）');
    chk(posts[0] !== undefined && posts[0].body && posts[0].body.on === true,
        'm04c: 请求体是 {on:true}，实得 ' + JSON.stringify(posts[0] && posts[0].body));

    console.log('m05 手动录像态');
    pageData = Object.assign({}, pageData, { recording: true, forced: true });
    nav.click();
    await sleep(120);
    chk(doc.getElementById('content').textContent.indexOf('手动启动') >= 0,
        'm05a: 结论写明是手动启动');
    chk(doc.getElementById('content').textContent.indexOf('需点「停止录像」才会停') >= 0,
        'm05b: 并且说清它不会随主网恢复自动停');

    console.log('m06 已人工停止（压制自动开录）');
    pageData = Object.assign({}, pageData, { recording: false, forced: false, held_off: true });
    nav.click();
    await sleep(120);
    chk(doc.getElementById('content').textContent.indexOf('已人工停止') >= 0,
        'm06a: 结论写明是人工停止，不是"没配好"');
    chk(doc.getElementById('content').textContent.indexOf('主网恢复一次后自动解除') >= 0,
        'm06b: 并说清解除条件（否则现场以为功能坏了）');

    console.log('m07 停止失败时把后端的原话给用户');
    postRsp = { ok: false, e: -20, msg: '摄像机端口未开放：192.168.1.250:554 拒绝连接' };
    /* ⚠ 每次 nav.click() 都会重建整页（`go()` 里 `content.innerHTML=''`），
     * 所以按钮必须**重新查**。拿旧引用点下去只会执行挂在废弃节点上的闭包，
     * 文档里什么都不会变 —— 断言会以一种很像"功能坏了"的方式红。 */
    const btnOff2 = byText(doc.getElementById('content'), 'button', '停止录像');

    chk(btnOff2 !== null, 'm07a0: 切页后「停止录像」按钮还在');
    if (btnOff2 !== null)
        btnOff2.click();
    await sleep(150);
    chk(posts.length === 2 && posts[1].body.on === false, 'm07a: 停止发 {on:false}');
    chk(doc.getElementById('content').textContent.indexOf('192.168.1.250:554') >= 0,
        'm07b: 失败原因原样出现在提示区（不许改写成"操作失败"）');

    console.log('m08 后端不可达时的文案');
    pageData = null; /* 让 GET 返回 ok:false 的形态 */
    window.fetch = function (u, o) {
        o = o || {};
        if (u.indexOf('/api/media/record') === 0) {
            if ((o.method || 'GET').toUpperCase() === 'POST')
                return Promise.resolve(rsp({ ok: true, d: { on: false, changed: false } }));
            return Promise.resolve(rsp({ ok: false, e: -95, msg: 'media not wired' }));
        }
        return Promise.resolve(rsp({ ok: true, d: {} }));
    };
    nav.click();
    await sleep(120);
    chk(doc.getElementById('content').textContent.indexOf('未装配媒体链') >= 0,
        'm08a: 媒体链没装配时说"未装配媒体链"，不是空白');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_media_record passed (25 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
