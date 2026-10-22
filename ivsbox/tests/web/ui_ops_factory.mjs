/*
 * 前端行为测试：运维页的「恢复出厂」卡（2026-10-22 单片机 web 对齐第 ⑥ 项）
 *
 * ============================ 为什么要有这个 ============================
 * 这一卡是整站**唯一会删现场数据**的按钮，前端这边承担三件事，全都在浏览器里、
 * `make test` / `make arm` / `node --check` 都碰不到：
 *   1. **擦除存储必须独立勾选、默认不勾**，不勾时请求体里**一个 `erase` 键都不许有**
 *      —— 少这一条，用户点"恢复出厂"就把录像删了（服务端按"传了才擦"实现，
 *      前端多传一个空数组也会被当成"要擦"）；
 *   2. **两道 confirm**：擦除这一路要问两遍（"确认恢复出厂"→"数据不可恢复"），
 *      任何一遍被拒都不许发请求；
 *   3. **成功后回登录页**：口令已回出厂、全部会话作废，留在主界面点什么都 401。
 *      提示语里必须说清"用出厂口令登录并立即改密"，否则现场会以为设备失联。
 *
 * 另有一条非功能断言：按钮要**防重入**（连点两下只下发一次）—— 恢复出厂是
 * 不可逆操作，重入一次就多存一遍配置、多擦一遍数据。
 *
 * 后端那一半（真的擦目录、真的写回出厂口令、真的踢会话）由 VM 上的真 HTTP
 * 端到端取证，见 docs/修改记录.md 2026-10-22「恢复出厂」条目。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_ops_factory.mjs
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

/* ---------------- confirm 桩 ----------------
 * jsdom 的 window.confirm 是 "not implemented"（会往虚拟控制台打一条错误并返回
 * undefined ⇒ 恒当"拒绝"）。必须自己接管：既能让用例决定答什么，也能数"问了几次"
 * —— "问两次"本身就是被测行为。 */
let confirmQueue = [];
const confirmMsgs = [];
window.confirm = function (msg) {
    confirmMsgs.push(String(msg));
    return confirmQueue.length ? confirmQueue.shift() : true;
};

/* ---------------- fetch 桩 ---------------- */
let facMode = 'ok';        /* ok | fail */
let facCalls = 0;
let facBody = null;
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

const FAC_OK = {
    ok: true,
    /* d 是 ctl `sys.factory_reset` 原样透传的那一份 */
    d: {
        cfg_changed: 37,
        version: 9,
        erase: ['media', 'log', 'db'],
        entries_removed: 128,
        db_rows_removed: 640,
        password_reset_required: true
    },
    /* 这两项是 ivsbox-web 自己加的，在**顶层**（不在 d 里） */
    password_reset: true,
    must_change: true
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
    if (u.indexOf('/api/factory_reset') === 0) {
        facCalls++;
        try { facBody = JSON.parse(o.body || '{}'); } catch (e) { facBody = {}; }
        if (facMode === 'fail')
            return Promise.resolve(rsp({ ok: false, e: -5, msg: 'cannot save config' }));
        return Promise.resolve(rsp(FAC_OK));
    }
    /* 运维页还有升级卡、日志卡与两张 configCard：真实后端恒给 `d.items`（对象，
     * 可能为空）。这里照实模拟，别让用例因为"别的卡挂了"而红。 */
    if (u.indexOf('/api/ota') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: {
                pending: false, active: true, state: 'idle', version: 'test',
                previous: 'test', bootcount: 1, max_boot: 3, rollbacks: 0,
                last_event: 'boot-ok', last_reason: '-', current: 'a'
            }
        }));
    }
    if (u.indexOf('/api/logs') === 0) {
        return Promise.resolve(rsp({ ok: true, d: { root: '/mnt/UDISK/log', files: [], total: 0, shown: 0 } }));
    }
    if (u.indexOf('/api/config') === 0) {
        return Promise.resolve(rsp({ ok: true, d: { version: 1, items: {} } }));
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

/* 每次重进运维页都重建 DOM（go() 会清 #content），拿到的卡片与按钮都是新的。
 * 顺带把"登录遮罩"复位成"已登录"—— 前面有用例会真的走成功路径（那会回登录页），
 * 不复位的话后面「失败不该踢回登录页」那条断言测的就是**上一个用例的残留状态**，
 * 恒绿且没有区分力（第一版就是这么假绿的，见 f06c）。 */
async function openCard() {
    doc.getElementById('login').className = 'login hidden';
    doc.getElementById('chpass').className = 'login hidden';
    doc.getElementById('app').className = 'app';

    const nav = byText(doc.getElementById('navList'), 'button', '运维');
    if (nav === null)
        return null;
    nav.click();
    await sleep(60);
    return cardByTitle(doc.getElementById('content'), '恢复出厂');
}

const btnOf = (card) => byText(card, 'button', '恢复出厂设置');
const chkOf = (card) => card.querySelector('input[type=checkbox]');

const main = async () => {
    await sleep(50);

    console.log('f01 卡片与默认状态');
    let card = await openCard();

    chk(card !== null, 'f01a: 运维页出现「恢复出厂」卡');
    if (card === null)
        return;
    chk(btnOf(card) !== null, 'f01b: 有「恢复出厂设置」按钮');
    chk(chkOf(card) !== null, 'f01c: 有"同时擦除存储"勾选框');
    chk(chkOf(card) !== null && chkOf(card).checked === false,
        'f01d: 勾选框**默认不勾**（删数据不能是默认行为）');
    chk(card.textContent.indexOf('devid') >= 0 || card.textContent.indexOf('MAC') >= 0,
        'f01e: 文案说明工厂身份不受影响');

    console.log('f02 不勾擦除：只恢复配置');
    facBody = null;
    facCalls = 0;
    confirmQueue = [true];
    confirmMsgs.length = 0;
    btnOf(card).click();
    await sleep(80);

    chk(confirmMsgs.length === 1, 'f02a: 只问一次（没勾擦除就不用问第二遍）');
    chk(facCalls === 1, 'f02b: 下发一次 POST /api/factory_reset');
    chk(facBody !== null && facBody.confirm === true, 'f02c: 请求体带 confirm:true');
    chk(facBody !== null && !('erase' in facBody),
        'f02d: 请求体**没有 erase 键**（服务端按"只恢复配置"处理）');

    console.log('f03 成功后回登录页');
    chk(doc.getElementById('login').className.indexOf('hidden') < 0,
        'f03a: 回登录页（#login 可见）');
    chk(doc.getElementById('app').className.indexOf('hidden') >= 0,
        'f03b: 主界面隐藏（旧会话已作废）');
    {
        const msg = doc.getElementById('loginMsg').textContent;

        chk(msg.indexOf('出厂口令') >= 0 && msg.indexOf('改密') >= 0,
            'f03c: 提示语要求用出厂口令登录并立即改密');
        chk(msg.indexOf('37') >= 0, 'f03d: 提示语带上回默认的配置项数');
        chk(msg.indexOf('删除') < 0, 'f03e: 没勾擦除就不该说删了多少东西');
    }

    console.log('f04 勾了擦除：三项 erase ＋ 两道确认');
    card = await openCard();
    if (card === null) { chk(false, 'f04: 重新进运维页'); return; }
    chkOf(card).checked = true;
    facBody = null;
    facCalls = 0;
    confirmQueue = [true, true];
    confirmMsgs.length = 0;
    btnOf(card).click();
    await sleep(80);

    chk(confirmMsgs.length === 2, 'f04a: 擦除路径问两次确认');
    chk(confirmMsgs[1].indexOf('永久删除') >= 0, 'f04b: 第二道确认明说"永久删除"');
    chk(facCalls === 1, 'f04c: 下发一次请求');
    chk(facBody !== null && Array.isArray(facBody.erase) &&
        facBody.erase.length === 3 &&
        facBody.erase[0] === 'media' && facBody.erase[1] === 'log' && facBody.erase[2] === 'db',
    'f04d: erase = ["media","log","db"]');
    {
        const msg = doc.getElementById('loginMsg').textContent;

        chk(msg.indexOf('128') >= 0 && msg.indexOf('640') >= 0,
            'f04e: 提示语带上删除的文件数与数据库行数');
    }

    console.log('f05 任一道确认被拒 = 不发请求');
    card = await openCard();
    if (card === null) { chk(false, 'f05: 重新进运维页'); return; }
    chkOf(card).checked = false;
    facCalls = 0;
    confirmQueue = [false];
    confirmMsgs.length = 0;
    btnOf(card).click();
    await sleep(60);
    chk(facCalls === 0, 'f05a: 第一道确认拒绝 → 一个请求都不发');

    chkOf(card).checked = true;
    facCalls = 0;
    confirmQueue = [true, false];
    confirmMsgs.length = 0;
    btnOf(card).click();
    await sleep(60);
    chk(confirmMsgs.length === 2 && facCalls === 0,
        'f05b: 第二道确认拒绝 → 一个请求都不发');

    console.log('f06 后端失败');
    card = await openCard();
    if (card === null) { chk(false, 'f06: 重新进运维页'); return; }
    chkOf(card).checked = false;
    facMode = 'fail';
    facCalls = 0;
    confirmQueue = [true];
    const toastEl = doc.getElementById('toast');

    toastEl.textContent = '';
    btnOf(card).click();
    await sleep(80);
    chk(facCalls === 1, 'f06a: 失败路径确实发了请求');
    chk(toastEl.textContent.indexOf('失败') >= 0, 'f06b: 失败时弹错误提示（不静默）');
    chk(doc.getElementById('login').className.indexOf('hidden') >= 0,
        'f06c: 失败**不能**把用户踢回登录页（会话还有效）');
    chk(btnOf(card).disabled === false, 'f06d: 失败后按钮恢复可用');
    facMode = 'ok';

    console.log('f07 防重入');
    card = await openCard();
    if (card === null) { chk(false, 'f07: 重新进运维页'); return; }
    facCalls = 0;
    confirmQueue = [true, true];
    btnOf(card).click();
    btnOf(card).click();  /* 连点：第二次必须被 facBusy 挡掉 */
    btnOf(card).click();
    await sleep(80);
    chk(facCalls === 1, 'f07a: 连点三下只下发一次（恢复出厂不可重入）');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_ops_factory passed (26 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
