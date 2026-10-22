/*
 * 前端行为测试：运维页的「系统升级」与「系统日志」两张卡
 *
 * ============================ 为什么要有这个 ============================
 * 这套 UI 全在浏览器里（web/static/app.js 的 pageMaint），`make test` / `make itest`
 * / `make arm` 都碰不到，`node --check` 只管语法。本用例用 jsdom 跑**真实的**
 * index.html + app.js，断言：
 *   - 运维页出现「系统升级」卡，字段取自 `/api/ota`（当前版本/上一版本/启动计数…）；
 *   - 有包待装与没包待装是**两句不同的话**（"没事干"与"读不到"必须能分开）；
 *   - 启动计数 > 1 加 `warn` 样式（回滚风险要肉眼可见）；
 *   - 「系统日志」卡列出 `/api/logs` 的文件，下载链指向 `/api/logs/file?f=`
 *     且 `f` 做过 URL 编码（日期里有 `/`，不编码会被浏览器当路径段截掉）；
 *   - 列表为空时明确说"没有日志文件"而不是留一片空白；
 *   - 后端回 `ok:false` 时卡片显示"不可用"，不静默吞掉。
 *
 * 后端那一半（真的扫目录、真的下发文件体）由 VM 上的真 HTTP 端到端取证，
 * 见 docs/修改记录.md 2026-10-22 条目。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_ops_log.mjs
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

/* ---------------- fetch 桩 ---------------- */
let logMode = 'some';   /* some | empty | broken */
let logCalls = 0;
let otaCalls = 0;
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

const OTA = {
    ok: true,
    d: {
        pending: false,
        active: true,
        state: 'idle',
        version: '1.0.0.20261022',
        previous: '1.0.0.20261020',
        bootcount: 2,
        max_boot: 3,
        rollbacks: 1,
        last_event: 'boot-ok',
        last_reason: '-',
        current: 'a'
    }
};

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
                    wall_sec: 1761000000, name: null
                },
                uptime_ms: 123456
            }
        }));
    }
    if (u.indexOf('/api/ota') === 0) {
        otaCalls++;
        return Promise.resolve(rsp(OTA));
    }
    /* 运维页底部还有两张 configCard（probe/netthr、elec/sensor），它们会查配置。
     * 真实后端恒给 `d.items`（对象，可能为空），这里必须照实模拟——否则本用例
     * 会因为"配置卡挂了"而失败，把注意力从被测的两张卡上引开。 */
    if (u.indexOf('/api/config') === 0) {
        return Promise.resolve(rsp({ ok: true, d: { version: 1, items: {} } }));
    }
    if (u.indexOf('/api/logs') === 0) {
        logCalls++;
        if (logMode === 'broken')
            return Promise.resolve(rsp({ ok: false, e: -7, msg: 'must change password' }));
        if (logMode === 'empty')
            return Promise.resolve(rsp({
                ok: true, d: { root: '/mnt/UDISK/log', files: [], total: 0, shown: 0 }
            }));
        return Promise.resolve(rsp({
            ok: true,
            d: {
                root: '/mnt/UDISK/log',
                total: 37,
                shown: 2,
                files: [
                    { f: '2026-10-22/08.log', size: 12345, mtime: 1761000000 },
                    { f: '2026-10-22/7.log', size: 900, mtime: 1760996400 }
                ]
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

function cardByTitle(root, title) {
    for (const c of root.querySelectorAll('.card')) {
        const h = c.querySelector('h3');
        if (h && h.textContent.trim() === title)
            return c;
    }
    return null;
}

const main = async () => {
    await sleep(50);

    console.log('o01 进入运维页');
    const nav = byText(doc.getElementById('navList'), 'button', '运维') ||
        byText(doc.getElementById('navList'), 'a', '运维');

    chk(nav !== null, 'o01a: 导航里有「运维」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(120);

    const content = doc.getElementById('content');
    const up = cardByTitle(content, '系统升级');
    const lg = cardByTitle(content, '系统日志');

    chk(up !== null, 'o01b: 出现「系统升级」卡');
    chk(lg !== null, 'o01c: 出现「系统日志」卡');
    if (up === null || lg === null)
        return;

    console.log('o02 升级状态字段');
    const upText = up.textContent;
    chk(otaCalls === 1, 'o02a: 进页只查一次 /api/ota');
    chk(upText.indexOf('1.0.0.20261022') >= 0, 'o02b: 显示当前版本');
    chk(upText.indexOf('1.0.0.20261020') >= 0, 'o02c: 显示上一版本');
    chk(upText.indexOf('当前没有待安装的包') >= 0, 'o02d: pending=false 时说"没有待安装的包"');
    chk(upText.indexOf('2 / 3') >= 0, 'o02e: 启动计数按 "n / max" 显示');
    {
        /* 启动计数 >1 必须带 warn：这是"回滚正在逼近"的唯一可见信号 */
        const warnCells = Array.from(up.querySelectorAll('.v.warn')).map((e) => e.textContent.trim());

        chk(warnCells.some((t) => t === '2 / 3'), 'o02f: 启动计数 >1 带 warn 样式');
        chk(warnCells.some((t) => t === '1'), 'o02g: 回滚次数 >0 带 warn 样式');
    }

    console.log('o03 日志列表');
    const links = Array.from(lg.querySelectorAll('a'));
    const lgText = lg.textContent;

    chk(logCalls === 1, 'o03a: 进页只查一次 /api/logs');
    chk(linkCount(links) , 'o03b: 每个文件一个下载链接（2 个）');
    chk(lgText.indexOf('2026-10-22/08.log') >= 0, 'o03c: 列出文件名');
    chk(links.length > 0 &&
        links[0].getAttribute('href') === '/api/logs/file?f=2026-10-22%2F08.log',
        'o03d: 下载链接对 f 做了 URL 编码（斜杠编成 %2F）');
    chk(lgText.indexOf('12 KB') >= 0, 'o03e: 文件大小按 KB 显示');
    chk(lgText.indexOf('37') >= 0, 'o03f: 提示里给出总数 37');

    console.log('o04 刷新按钮');
    const bRefresh = byText(lg, 'button', '刷新日志列表');

    chk(bRefresh !== null, 'o04a: 有「刷新日志列表」按钮');
    if (bRefresh !== null) {
        bRefresh.click();
        await sleep(80);
        chk(logCalls === 2, 'o04b: 点刷新会再查一次（不是只渲染一次就完事）');
    }

    console.log('o05 空列表');
    logMode = 'empty';
    if (bRefresh !== null) {
        bRefresh.click();
        await sleep(80);
    }
    chk(lg.textContent.indexOf('没有日志文件') >= 0, 'o05a: 空列表明确说"没有日志文件"');
    chk(lg.querySelectorAll('a').length === 0, 'o05b: 空列表不留死链接');

    console.log('o06 后端报错');
    logMode = 'broken';
    if (bRefresh !== null) {
        bRefresh.click();
        await sleep(80);
    }
    chk(lg.textContent.indexOf('不可用') >= 0, 'o06a: ok:false 时卡片显示"不可用"而不是空白');
};

/* 只数"下载"链接，别把页面里别的 a 混进来 */
function linkCount(links) {
    return links.filter((a) => a.textContent.trim() === '下载').length === 2;
}

main().then(() => {
    console.log(fail === 0 ? 'ui_ops_log passed (16 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
