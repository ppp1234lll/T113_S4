/*
 * 前端行为测试：会话过期（HTTP 401）必须回登录页并说明原因
 *
 * ============================ 为什么要有这个 ============================
 * 2026-10-29 早上现场：Web 页面「网络」页三块只读信息全是
 *   「未获取到链路状态（ivsboxd 未就绪或网络模块未装配）」
 *   「未获取到平台通信状态（…）」「未获取到网口信息」
 *     「读取失败：unauthorized」
 * 设备本身是好的（curl 带有效 token 取 `/api/overview` 200、`net.link.ifaces` 两个网口
 * 数据齐全）。真因是**会话过期**：`IV_API_SESS_TTL_MS` 8 h，原先登录后不续期；页面
 * 放在那儿过一夜，第二天点任何按钮/切页都是 401 `{"ok":false,"e":-23,"msg":"unauthorized"}`。
 * 而 `api()` 只接管了 403（出厂默认口令），**没接管 401** ⇒ 每个调用点各自"静默失败"，
 * 页面就长成上面那样 —— 用户只能理解成"设备坏了"。
 *
 * 本用例钉住三件事（都在浏览器里，`make test`/`itest`/`arm` 都碰不到）：
 *   s01 会话有效时正常进主界面（先决条件，否则后面几条是空转）；
 *   s02 **开着页面掉线**：切到「网络」页触发一次 401 ⇒ 回登录卡片 ＋ 说明"登录已过期"，
 *       且**不能**串到改密卡片（401 与 403 是两条路）；
 *   s03 登录接口自己的 401（`bad password`）**不能**被当成"登录已过期"抢走提示。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_session_401.mjs
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
const hidden = (id) => (doc.getElementById(id).className || '').indexOf('hidden') >= 0;

/* ---------------- fetch 桩：先正常，再整体切成 401 ---------------- */
let expired = false;
let loginBad = false;
const rsp = (status, obj) => ({ status, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    if (u.indexOf('/api/login') === 0 && loginBad)
        return Promise.resolve(rsp(401, { ok: false, e: -23, msg: 'bad password' }));
    if (expired)
        return Promise.resolve(rsp(401, { ok: false, e: -23, msg: 'unauthorized' }));
    if (u.indexOf('/api/overview') === 0) {
        return Promise.resolve(rsp(200, {
            ok: true,
            d: {
                version: 'test',
                device: { name: null, devtype: '0a00', sysver: '1.0.0.20261028',
                          hw_model: 'FN_RZTY_PY', model: 'embedsky,tqt113-core' },
                net: {
                    state: 'WIRED_UP', mn: [1, 1], active_wan: 'wired',
                    link: {
                        gateway: '192.168.1.1', gateway_if: 'eth0',
                        ifaces: [
                            { name: 'eth0', present: true, up: true, carrier: true,
                              ip: '192.168.1.120', mask: '255.255.255.0',
                              mac: '2e:a2:9e:ef:43:43' }
                        ]
                    },
                    wan: { ifname: 'eth0', kind: 'wired', transport: 'up', bound: 'eth0',
                           bound_ok: true, server: '47.104.250.225', port: 6013,
                           queue: 0, switches: 0, txn_fail: 0 }
                }
            }
        }));
    }
    if (u.indexOf('/api/config?') === 0)
        return Promise.resolve(rsp(200, { ok: true, d: { version: 1, items: {} } }));
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

const main = async () => {
    await sleep(50);

    /* ---- s01 先决条件：会话有效时能进主界面 ---- */
    console.log('s01 会话有效 → 主界面');
    chk(!hidden('app'), 's01a: 首屏进到主界面');
    chk(hidden('login'), 's01b: 登录卡片隐藏');
    chk(doc.getElementById('loginMsg').textContent === '',
        's01c: 登录提示为空（没有残留文案）');

    /* ---- s02 开着页面掉线：切到「网络」页，后端全 401 ---- */
    console.log('s02 页面开着掉线 → 回登录页并说明原因');
    expired = true;
    const nav = byText(doc.getElementById('navList'), 'button', '网络');
    chk(nav !== null, 's02a: 导航里有「网络」入口');
    if (nav === null) {
        console.log('ui_session_401 FAILED (' + fail + ' checks)');
        process.exit(1);
    }
    nav.click();
    await sleep(150);

    chk(!hidden('login'), 's02b: 回到登录卡片（不再停在一堆占位文案上）');
    chk(hidden('app'), 's02c: 主界面隐藏');
    chk(doc.getElementById('loginMsg').textContent.indexOf('登录已过期') >= 0,
        's02d: 登录卡片说明"登录已过期"（不是空白，也不是设备故障话术）');
    /* 401 与 403 是两条路：出厂默认口令才切改密卡片，这里不许串 */
    chk(hidden('chpass'), 's02e: 没有串到改密卡片');

    /* ---- s03 登录接口自己的 401（密码错）不许被"登录已过期"抢走 ---- */
    console.log('s03 登录失败的提示归登录卡片');
    expired = false;
    loginBad = true;
    const loginForm = doc.getElementById('loginForm');
    doc.getElementById('user').value = 'admin';
    doc.getElementById('pass').value = 'wrong-pass';
    loginForm.dispatchEvent(new window.Event('submit', { cancelable: true, bubbles: true }));
    await sleep(120);
    const lm = doc.getElementById('loginMsg').textContent;
    chk(lm.indexOf('账号或密码不正确') >= 0,
        's03a: 密码错显示"账号或密码不正确"');
    chk(lm.indexOf('登录已过期') < 0,
        's03b: 没有被 401 统一接管盖成"登录已过期"');
    chk(!hidden('login'), 's03c: 仍停在登录卡片');

    if (fail === 0) {
        console.log('ui_session_401: all checks passed');
        process.exit(0);
    }
    console.log('ui_session_401 FAILED (' + fail + ' checks)');
    process.exit(1);
};

main();
