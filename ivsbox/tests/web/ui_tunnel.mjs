/*
 * 前端行为测试：「远程通道」页（frpc 参数 + 令牌 + 开关）
 *
 * ============================ 为什么要有这个 ============================
 * 这条功能一半在浏览器里（`web/static/app.js` 的 pageTunnel），`make test` /
 * `make itest` / `make arm` 一个都碰不到它。页面本身只有两个动作，但两个动作的
 * **顺序和 body 形状**都是硬约束，写错了现场不会报错，只会"点了保存、通道没开"：
 *
 *   1. 服务器落点走普通配置（`/api/config/apply`，要带**现取**的版本号），
 *      令牌与开关走 `/api/tunnel/config`（一次交付：落盘 → 换钥匙 → 开/关）。
 *      顺序反了就是"令牌写进去了、frps 还是旧地址"。
 *   2. **只提交变了的键**：地址没改就不发 config.apply —— 白提交一次会白白
 *      推进版本号，别的页正拿着旧版本时下一次保存必撞 e=-3。
 *   3. **令牌留空 ＝ 不改**，请求体里不能出现 `token` 键。写成 `token:''` 就等于
 *      "把设备令牌清空"，而那会让通道再也开不起来（fail-closed），且界面上
 *      看不出发生过什么。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_tunnel.mjs
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

const dom = new JSDOM(fs.readFileSync(htmlPath, 'utf8'),
                      { runScripts: 'outside-only', url: 'http://127.0.0.1/' });
const { window } = dom;
const doc = window.document;

let fail = 0;
function chk(cond, what) {
    if (cond) console.log('  PASS ' + what);
    else { console.log('  FAIL ' + what); fail++; }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---------------- fetch 桩 ---------------- */
let ver = 7;                      /* 故意的非 0 起点：能证明"版本号是现取的" */
let host = 'frp.old.example';
let port = 0;
let on = false;
let tokenSet = false;
let ttl = 3600;
const applyCalls = [];            /* /api/config/apply 的 body */
const tunCalls = [];              /* /api/tunnel/config 的 body */
const disCalls = [];

const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

function status() {
    return {
        ok: true,
        d: {
            on: on, remain_s: on ? 3000 : 0, ttl_s: ttl,
            sn: 'A19B5', server: host, server_port: port || 7000,
            token_set: tokenSet, token_len: tokenSet ? 32 : 0,
            local_port: 22, conf: '/etc/frp/frpc.toml', flag: '/tmp/frp.enabled',
            stat: { opens: 1, denies: 0, expires: 0, uptime_s: 60 },
            note: ''
        }
    };
}

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/tunnel/config') === 0) {
        const body = JSON.parse(o.body);

        tunCalls.push(body);
        if (body.token) { tokenSet = true; }
        if (typeof body.enable === 'boolean') on = body.enable;
        if (body.enable) ttl = Number(body.ttl) || 0;
        return Promise.resolve(rsp({ ok: true, d: { on: on, ttl_s: ttl, remain_s: on ? ttl : 0,
                                                    token_set: tokenSet, sn: 'A19B5' } }));
    }
    if (u.indexOf('/api/tunnel/disable') === 0) {
        disCalls.push(JSON.parse(o.body || '{}'));
        on = false;
        return Promise.resolve(rsp({ ok: true, d: { on: false, stop: 'local' } }));
    }
    if (u.indexOf('/api/tunnel') === 0)
        return Promise.resolve(rsp(status()));
    if (u.indexOf('/api/config?defaults=1') === 0) {
        /* 「全部配置」页要读出厂默认值表（t07 会切到那页）。这里只回我们关心的
         * 四个键 —— 桩不追求像真后端，只求页面别在别的键上炸掉。 */
        return Promise.resolve(rsp({
            ok: true,
            d: { items: {
                'sys.tunnel.server.host': { t: 'str', v: '' },
                'sys.tunnel.server.port': { t: 'int', v: 0 },
                'sys.device.model': { t: 'str', v: 'FN_RZTY_PY' },
                'sys.device.name': { t: 'str', v: '' }
            } }
        }));
    }
    if (u.indexOf('/api/config?cls=sys') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: { version: ver, items: {
                'sys.tunnel.server.host': host,
                'sys.tunnel.server.port': port,
                'sys.device.model': 'FN_RZTY_PY'
            } }
        }));
    }
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        if (body.ver !== ver)
            return Promise.resolve(rsp({ ok: false, e: -3, msg: 'version conflict' }));
        Object.keys(body.items).forEach((k) => {
            if (k === 'sys.tunnel.server.host') host = body.items[k];
            if (k === 'sys.tunnel.server.port') port = body.items[k];
        });
        ver += 1;
        return Promise.resolve(rsp({ ok: true, d: { version: ver, applied: Object.keys(body.items).length } }));
    }
    /* 其余分类（net/probe/camera…）在 t07 里会被通用配置卡逐个读一次。
     * 回空 items 且带 version —— 少一个字段前端就会在 Object.keys 上炸掉。 */
    if (u.indexOf('/api/config?cls=') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    return Promise.resolve(rsp({ ok: true, d: {} }));
};

window.eval(fs.readFileSync(appPath, 'utf8'));
if (doc.readyState === 'loading')
    doc.dispatchEvent(new window.Event('DOMContentLoaded'));

function byText(root, tag, text) {
    for (const e of root.querySelectorAll(tag)) {
        if (e.textContent.trim() === text) return e;
    }
    return null;
}

const main = async () => {
    await sleep(50);

    console.log('t01 进入「远程通道」页');
    const nav = byText(doc.getElementById('navList'), 'button', '远程通道');
    chk(nav !== null, 't01a: 导航里有「远程通道」入口');
    if (nav === null) return;
    nav.click();
    await sleep(120);

    const content = doc.getElementById('content');
    const save = byText(content, 'button', '保存并生效');
    const gen = byText(content, 'button', '生成随机令牌');
    const off = byText(content, 'button', '关闭通道');
    const texts = Array.from(content.querySelectorAll('input[type="text"]'));

    chk(save !== null, 't01b: 出现「保存并生效」按钮');
    chk(gen !== null, 't01c: 出现「生成随机令牌」按钮');
    chk(off !== null, 't01d: 出现「关闭通道」按钮');
    if (save === null || gen === null || off === null) return;

    console.log('t02 地址/端口回填与状态渲染');
    const hostInp = texts.find((i) => /frps 域名或 IP/.test(i.placeholder || ''));
    const portInp = content.querySelector('input[type="number"]');
    const tokInp = content.querySelector('input[type="password"]');
    const chkEn = content.querySelector('input[type="checkbox"]');

    chk(hostInp !== undefined && hostInp.value === 'frp.old.example',
        't02a: frps 地址从 sys 类配置回填');
    chk(portInp !== null && portInp.value === '0', 't02b: 端口回填（0 表示默认 7000）');
    chk(tokInp !== null && tokInp.value === '', 't02c: 令牌框初始为空（设备令牌不回显）');
    chk(content.textContent.indexOf('未配置') >= 0, 't02d: 状态卡显示「令牌未配置」');
    chk(content.textContent.indexOf('A19B5') >= 0, 't02e: 状态卡显示设备 SN');
    chk(chkEn !== null && chkEn.checked === false, 't02f: 未开启时「启用通道」不勾');

    console.log('t03 生成随机令牌');
    gen.click();
    await sleep(30);
    const tk = tokInp.value;

    chk(/^[0-9a-f]{32}$/.test(tk), 't03a: 生成 32 位小写 hex，实得「' + tk + '」');
    chk(tokInp.type === 'text', 't03b: 生成后明文显示（方便抄到运维 PC）');

    console.log('t04 地址未改，勾启用并保存');
    chkEn.checked = true;
    save.click();
    await sleep(150);

    chk(applyCalls.length === 0, 't04a: 地址/端口没改 ⇒ 不发 config.apply（不白推版本号）');
    chk(tunCalls.length === 1, 't04b: 只发一次 /api/tunnel/config');
    const b1 = tunCalls[0] || {};

    chk(b1.token === tk, 't04c: 带上令牌');
    chk(b1.enable === true, 't04d: enable=true');
    chk(Number(b1.ttl) === 3600, 't04e: 有效期跟随下拉现值（3600）');
    chk(tokInp.value === '', 't04f: 保存成功后清空令牌框（避免下次误提交）');

    console.log('t05 改地址 ⇒ 先 config.apply 再 tunnel/config');
    hostInp.value = 'frp.new.example';
    tokInp.value = '';
    save.click();
    await sleep(150);

    chk(applyCalls.length === 1, 't05a: 地址变了才发 config.apply');
    const a1 = applyCalls[0] || { items: {} };

    chk(a1.items['sys.tunnel.server.host'] === 'frp.new.example', 't05b: 只提交改了的 host');
    chk(a1.ver === 7, 't05c: 版本号是现取的（=7，不是缓存的 0）');
    chk(Object.keys(a1.items).indexOf('sys.tunnel.server.port') < 0,
        't05d: 端口没变就不提交该键');
    chk(tunCalls.length === 2, 't05e: 地址保存成功后才发 tunnel/config');
    chk(tunCalls[1].token === undefined,
        't05f: 令牌留空时**不带** token 键（留空＝不改，不是清空）');

    console.log('t06 关闭通道');
    /* 页面用的是 window.confirm；jsdom 的 confirm 默认返回 false ⇒ 先改成 true，
     * 否则这条永远「点了没反应」，而真实浏览器里是能过的。 */
    window.confirm = () => true;
    off.click();
    await sleep(150);
    chk(disCalls.length === 1, 't06a: 点「关闭通道」发 /api/tunnel/disable');

    console.log('t07 「全部配置」页不再重复画这四个键');
    const cfgNav = byText(doc.getElementById('navList'), 'button', '配置');
    cfgNav.click();
    await sleep(120);
    /* 判据是**有没有可编辑的输入框**（[data-key]），不是页面上有没有出现这串键名：
     * 下面「出厂默认值对照」表本来就会列出全部键（那是只读文档，改不了），
     * 真正会导致"两个编辑器写同一批键 ⇒ 必然撞 e=-3"的只有 data-key 那一套。 */
    const dup = Array.from(doc.getElementById('content').querySelectorAll('[data-key]'))
        .filter((e) => (e.dataset.key || '').indexOf('sys.tunnel.') === 0);

    chk(dup.length === 0, 't07a: 通用配置页没有 sys.tunnel.* 的输入框（实得 ' + dup.length + ' 个）');
};

main().then(() => {
    console.log(fail === 0 ? 'ui_tunnel passed (26 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
