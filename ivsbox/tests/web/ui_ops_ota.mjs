/*
 * 前端行为测试：运维页「系统升级」卡里的**分片上传 / 续传 / 请求安装**
 *
 * ============================ 为什么要有这个 ============================
 * 上传逻辑全在浏览器里（`web/static/app.js` 的 pageMaint），`make test` / `make itest`
 * / `make arm` 都碰不到，`node --check` 只管语法。而这段代码最容易错的恰恰是**分寸**：
 *   - 每片到底多大（服务端硬上限 24 KiB，超一片就 413，包永远传不完）；
 *   - `off` 必须是"已上传字节数"，写成 0 就每片都覆盖第一片（服务端 e2e 抓到过一次
 *     服务端侧同样的错，见 `tests/web/ota_e2e.py` 的 lseek 修正）；
 *   - 服务端回 409 带实际偏移时**必须按服务端的值接**，不能自己猜；
 *   - 一次只能有一个上传在跑（防重入），失败/取消后按钮要能再点。
 * 这些都能在 jsdom 里用真 index.html + 真 app.js 断言，不需要设备。
 *
 * 后端那一半（真的落盘、真的校验、真的落请求文件）由两个真进程用例取证：
 *   tests/web/ota_e2e.py            —— 只有 ivsbox-web
 *   tests/integration/ota_install_e2e.py —— ivsbox-web ＋ ivsboxd（验 monitor 请求文件）
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_ops_ota.mjs
 */
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const webDir = path.resolve(here, '../../web/static');

let JSDOM;
try {
    const req = createRequire(path.join(here, 'noop.js'));
    ({ JSDOM } = req('jsdom'));
} catch (e) {
    console.log('SKIP: jsdom not installed (npm i jsdom, or set NODE_PATH); ' + e.message);
    process.exit(0);
}

const src = fs.readFileSync(path.join(webDir, 'app.js'), 'utf8');
const html = fs.readFileSync(path.join(webDir, 'index.html'), 'utf8');

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
const rsp = (status, obj) => ({
    status,
    text: () => Promise.resolve(JSON.stringify(obj))
});

/* ---------------- fetch 桩 ---------------- */
let chunkPosts = [];        /* 每次 POST /api/ota/chunk 的 {name, off, size, ctype} */
let finishCalls = [];
let installCalls = [];
let abortCalls = [];
let chunkGets = 0;
let resumeOff = 0;          /* GET /api/ota/chunk 回的续传点 */
let forceConflict = null;   /* 非 null ⇒ 下一次 POST 回 409 + 这个 off */
let finishOk = true;
/* 桩默认瞬时完成 ⇒ 3 片 + finish + install 在几十毫秒内全跑完，"取消"根本来不及点。
 * 这段延迟只给"取消上传"那条用例。 */
let chunkDelayMs = 0;

window.fetch = function (u, o) {
    o = o || {};
    const method = o.method || 'GET';

    if (u.indexOf('/api/overview') === 0) {
        return Promise.resolve(rsp(200, {
            ok: true,
            d: {
                version: 'test',
                device: {
                    version: 'test', model: 'FN_RZTY_PY', soc: 'sun8iw20p1',
                    sn: '1', sn_source: 'default', id_mac: null, eth0_mac: null,
                    hostname: 'ivsbox', kernel: '5.4.61-rt37', machine: 'armv7l',
                    wall_sec: 1761000000, name: null
                },
                uptime_ms: 1000
            }
        }));
    }
    if (u.indexOf('/api/ota/chunk') === 0) {
        const q = u.split('?')[1] || '';
        const kv = {};
        q.split('&').forEach((p) => {
            const i = p.indexOf('=');
            if (i > 0) kv[p.slice(0, i)] = decodeURIComponent(p.slice(i + 1));
        });
        if (method === 'GET') {
            chunkGets++;
            return Promise.resolve(rsp(200, { ok: true, d: { name: kv.name, off: resumeOff } }));
        }
        const size = o.body && typeof o.body.size === 'number' ? o.body.size : -1;
        chunkPosts.push({ name: kv.name, off: Number(kv.off), size, ctype: (o.headers || {})['Content-Type'] });
        if (chunkDelayMs > 0)
            return new Promise((res) => setTimeout(() => res(rsp(200, {
                ok: true, d: { name: kv.name, off: Number(kv.off) + size, next: Number(kv.off) + size }
            })), chunkDelayMs));
        if (forceConflict !== null) {
            const at = forceConflict;
            forceConflict = null;
            return Promise.resolve(rsp(409, {
                ok: false, e: -7, msg: 'offset mismatch', d: { name: kv.name, off: at }
            }));
        }
        return Promise.resolve(rsp(200, {
            ok: true, d: { name: kv.name, off: Number(kv.off) + size, next: Number(kv.off) + size }
        }));
    }
    if (u.indexOf('/api/ota/finish') === 0) {
        finishCalls.push(JSON.parse(o.body || '{}'));
        if (!finishOk) {
            return Promise.resolve(rsp(409, {
                ok: false, e: -43, msg: 'sha256 mismatch', d: { sha256: 'ff' }
            }));
        }
        return Promise.resolve(rsp(200, {
            ok: true,
            d: { name: 'x', path: '/mnt/UDISK/ivsbox/ota/x.pkg', size: 60000, sha256: 'ab'.repeat(32) }
        }));
    }
    if (u.indexOf('/api/ota/install') === 0) {
        installCalls.push(JSON.parse(o.body || '{}'));
        return Promise.resolve(rsp(200, { ok: true, d: { accepted: true, restart: true } }));
    }
    if (u.indexOf('/api/ota/abort') === 0) {
        abortCalls.push(JSON.parse(o.body || '{}'));
        return Promise.resolve(rsp(200, { ok: true, d: { aborted: true } }));
    }
    if (u.indexOf('/api/ota') === 0) {
        return Promise.resolve(rsp(200, {
            ok: true,
            d: {
                pending: false, state: 'idle', version: '1.0.0.x', previous: '1.0.0.y',
                bootcount: 0, max_boot: 3, rollbacks: 0, last_event: '-', last_reason: '-',
                current: 'a'
            }
        }));
    }
    if (u.indexOf('/api/config') === 0)
        return Promise.resolve(rsp(200, { ok: true, d: { version: 1, items: {} } }));
    if (u.indexOf('/api/logs') === 0)
        return Promise.resolve(rsp(200, { ok: true, d: { root: '/x', files: [], total: 0, shown: 0 } }));
    return Promise.resolve(rsp(200, { ok: true, d: {} }));
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

function cardByTitle(root, title) {
    for (const c of root.querySelectorAll('.card')) {
        const h = c.querySelector('h3');
        if (h && h.textContent.trim() === title)
            return c;
    }
    return null;
}

function pickFile(input, name, size) {
    const f = new window.File([new Uint8Array(size)], name);
    Object.defineProperty(input, 'files', { value: [f], configurable: true });
    input.dispatchEvent(new window.Event('change'));
    return f;
}

const main = async () => {
    await sleep(50);

    console.log('p01 进入运维页');
    const nav = byText(doc.getElementById('navList'), 'button', '运维') ||
        byText(doc.getElementById('navList'), 'a', '运维');
    chk(nav !== null, 'p01a: 导航里有「运维」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(120);

    const content = doc.getElementById('content');
    const up = cardByTitle(content, '系统升级');
    chk(up !== null, 'p01b: 出现「系统升级」卡');
    if (up === null)
        return;

    const input = up.querySelector('#ota-file');
    const bSend = byText(up, 'button', '上传并请求安装');
    const bAbort = byText(up, 'button', '取消上传');
    chk(input !== null && input.type === 'file', 'p01c: 有 file 选择框');
    chk(bSend !== null && bAbort !== null, 'p01d: 有「上传并请求安装」与「取消上传」');
    if (!input || !bSend || !bAbort)
        return;
    chk(bSend.disabled === true && bAbort.disabled === true,
        'p01e: 未选文件时两个按钮都禁用（不给按了没反应的机会）');

    console.log('p02 没选文件点发送');
    bSend.click();
    await sleep(30);
    chk(chunkGets === 0 && chunkPosts.length === 0, 'p02a: 不发任何请求');

    console.log('p03 选文件');
    pickFile(input, 'ivsbox-1.0.1.pkg', 60000);
    await sleep(30);
    chk(up.textContent.indexOf('ivsbox-1.0.1.pkg') >= 0, 'p03a: 显示已选文件名');
    chk(up.textContent.indexOf('59 KB') >= 0, 'p03b: 显示大小（60000 B ⇒ 59 KB）');
    chk(bSend.disabled === false, 'p03c: 选好文件后发送按钮可用');

    console.log('p04 上传三片');
    resumeOff = 0;
    bSend.click();
    await sleep(200);
    chk(chunkGets === 1, 'p04a: 先查一次续传点（GET /api/ota/chunk）');
    chk(chunkPosts.length === 3, `p04b: 60000 B 分 3 片（实得 ${chunkPosts.length}）`);
    chk(chunkPosts[0] && chunkPosts[0].off === 0 && chunkPosts[0].size === 24576,
        `p04c: 第一片 off=0、24576 B（实得 off=${chunkPosts[0] && chunkPosts[0].off} size=${chunkPosts[0] && chunkPosts[0].size}）`);
    chk(chunkPosts[1] && chunkPosts[1].off === 24576, 'p04d: 第二片 off=24576（不是又写 0）');
    chk(chunkPosts[2] && chunkPosts[2].off === 49152 && chunkPosts[2].size === 10848,
        'p04e: 第三片 off=49152、10848 B（尾片不是整片）');
    chk(chunkPosts.every((p) => p.ctype === 'application/octet-stream'),
        'p04f: 分片体按二进制发（带 octet-stream，不当 JSON 序列化）');
    chk(chunkPosts.every((p) => p.name === 'ivsbox-1.0.1.pkg'), 'p04g: 每片都带同一个包名');

    console.log('p05 finish / install');
    chk(finishCalls.length === 1, 'p05a: 传完调一次 /api/ota/finish');
    chk(finishCalls[0] && finishCalls[0].name === 'ivsbox-1.0.1.pkg' &&
        finishCalls[0].size === 60000, 'p05b: finish 带 name 与总字节数');
    chk(finishCalls[0] && finishCalls[0].sha256 === undefined,
        'p05c: 不带 sha256（纯 http 下浏览器拿不到 crypto.subtle，摘要交服务端算）');
    chk(installCalls.length === 1, 'p05d: finish 成功后调一次 /api/ota/install');
    chk(installCalls[0] && installCalls[0].name === 'ivsbox-1.0.1.pkg' &&
        installCalls[0].restart === 1, 'p05e: install 带 name 与 restart=1');
    chk(up.textContent.indexOf('已请求安装') >= 0, 'p05f: 明确告知"已请求安装"，不谎称"已升级"');
    chk(bSend.disabled === false && bAbort.disabled === true, 'p05g: 结束后按钮复位');

    console.log('p06 包名含特殊字符');
    chunkPosts = [];
    finishCalls = [];
    installCalls = [];
    pickFile(input, '升级 包.pkg', 100);
    await sleep(30);
    bSend.click();
    await sleep(150);
    /* '升级 包.pkg' ⇒ 升/级/空格/包 四个字符各换一个 `_`，其余原样 */
    chk(chunkPosts.length === 1 && chunkPosts[0].name === '____.pkg',
        `p06a: 中文/空格被换成安全名再上传（实得 ${chunkPosts[0] && chunkPosts[0].name}）`);

    console.log('p07 服务端说偏移对不上');
    chunkPosts = [];
    finishCalls = [];
    installCalls = [];
    resumeOff = 0;
    forceConflict = 24576;
    pickFile(input, 'big.pkg', 60000);
    await sleep(30);
    bSend.click();
    await sleep(250);
    chk(chunkPosts.length >= 2 && chunkPosts[0].off === 0, 'p07a: 第一片照常发');
    chk(chunkPosts.length >= 2 && chunkPosts[1].off === 24576,
        `p07b: 409 后按**服务端给的实际偏移**接（实得 ${chunkPosts[1] && chunkPosts[1].off}）`);
    chk(finishCalls.length === 1, 'p07c: 最终仍然传完并 finish');

    console.log('p08 finish 失败');
    chunkPosts = [];
    finishCalls = [];
    installCalls = [];
    resumeOff = 0;
    finishOk = false;
    pickFile(input, 'bad.pkg', 100);
    await sleep(30);
    bSend.click();
    await sleep(150);
    chk(up.textContent.indexOf('包校验失败') >= 0, 'p08a: 提示"包校验失败"');
    chk(installCalls.length === 0, 'p08b: 校验没过**不发**安装请求');
    finishOk = true;

    console.log('p09 取消上传');
    chunkPosts = [];
    abortCalls = [];
    installCalls = [];
    resumeOff = 0;
    chunkDelayMs = 300;   /* 让第一片"还在路上"，否则整个上传几十毫秒就跑完了 */
    pickFile(input, 'cancel.pkg', 60000);
    await sleep(30);
    bSend.click();
    await sleep(60);
    bAbort.click();
    await sleep(250);
    chunkDelayMs = 0;
    chk(abortCalls.length === 1 && abortCalls[0].name === 'cancel.pkg',
        'p09a: 取消会调 /api/ota/abort 丢掉未完成分片');
    chk(up.textContent.indexOf('已取消') >= 0, 'p09b: 提示已取消');
    chk(installCalls.length === 0, 'p09c: 取消后不发安装请求');
    chk(bSend.disabled === false, 'p09d: 取消后还能重新开始');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_ops_ota passed (30 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
