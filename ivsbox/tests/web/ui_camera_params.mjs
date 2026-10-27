/*
 * 前端行为测试：摄像机通道参数卡片（2026-10-27 用户口径）
 *
 * ============================ 为什么要有这个 ============================
 * 这张卡片整个在浏览器里（web/static/app.js），`make test`/`make arm`/`make itest`
 * **全都碰不到它**；只做 `node --check` 只能证明语法对。而这次改动最容易错的三处
 * 恰恰都不报错、只是"看起来对"：
 *   ① **拉流地址的拼装规则**（品牌→路径、主/子码流、有没有账号密码）——拼错了地址
 *      仍然是个合法的 RTSP 串，界面不报错，录像侧拉不到流才发现；
 *   ② **"手改过的地址不许被联动覆盖"** —— 写成无条件覆盖时，现场手调的地址会在
 *      下一次改密码时被静默冲掉；
 *   ③ **MAC 不给入口** —— 通用表单还会画它，改漏一处就在"全部配置"页复活。
 *
 * 做法：用 jsdom 把真实的 index.html + app.js 跑起来，桩住 /api/config*，
 * 然后按现场操作顺序点：改 IP → 填账号密码 → 切品牌 → 切码流 → 手改地址 → 保存。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_camera_params.mjs
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

/*
 * 一处 jsdom 与浏览器的已知差异必须在这里补桩：jsdom **不实现** HTMLSelectElement
 * 的选项自动选中（`select.value = ''` 在某些版本上不落到对应 option），
 * 于是 `sel.value` 可能回落到第一个 option 而不是空串 —— 而本用例要断言的
 * "未设置＝默认海康"正是靠空串。这里显式给 select.value 一个"按值找 option"
 * 的实现，保证被测代码看到的是浏览器里那套语义。
 */
const dom = new JSDOM(html, { runScripts: 'outside-only', url: 'http://127.0.0.1/' });
const { window } = dom;
const doc = window.document;
{
    const proto = window.HTMLSelectElement && window.HTMLSelectElement.prototype;

    if (proto) {
        const d = Object.getOwnPropertyDescriptor(proto, 'value');

        if (!d || !d.set) {
            Object.defineProperty(proto, 'value', {
                configurable: true,
                get() {
                    const o = this.querySelector('option[selected]');

                    return o ? o.value : (this.options.length ? this.options[0].value : '');
                },
                set(v) {
                    for (const o of this.options)
                        o.selected = (o.value === String(v));
                },
            });
        }
    }
}

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
let items = {
    'camera.ch.0.ip': '192.168.1.64',
    'camera.ch.0.mac': '',
    'camera.ch.0.user': '',
    'camera.ch.0.pass': '',
    'camera.ch.0.record': false,
    'camera.ch.0.stream': '',
    'camera.ch.0.url': '',
    'camera.ch.0.vendor': '',
    'camera.detect.mode': 1,
    'camera.detect.rtt_ms': 200,
};
let ver = 3;
const applyCalls = [];
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 'test', cameras: { cns: [] } } }));
    if (u.indexOf('/api/config?defaults=1') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: {
                version: ver,
                items: {
                    'camera.ch.0.mac': { t: 'str', d: '第 1 路 MAC', v: '' },
                    'camera.ch.0.ip': { t: 'str', d: '第 1 路 IP', v: '' },
                },
            },
        }));
    }
    if (u.indexOf('/api/config?cls=') === 0) {
        const cls = u.split('cls=')[1];

        return Promise.resolve(rsp({
            ok: true,
            d: {
                version: ver,
                items: cls === 'camera' ? items : {},
            },
        }));
    }
    if (u.indexOf('/api/snmp') === 0)
        return Promise.resolve(rsp({ ok: true, d: { state: 2, slots: [] } }));
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        Object.keys(body.items).forEach((k) => { items[k] = body.items[k]; });
        ver += 1;
        return Promise.resolve(rsp({
            ok: true,
            d: { version: ver, applied: Object.keys(body.items).length },
        }));
    }
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
const key = (k) => doc.querySelector('[data-key="' + k + '"]');
const fire = (node, type) => node.dispatchEvent(new window.Event(type));

const main = async () => {
    await sleep(50);

    console.log('p01 进入摄像机页');
    const nav = byText(doc.getElementById('navList'), 'button', '摄像机') ||
        byText(doc.getElementById('navList'), 'a', '摄像机');

    chk(nav !== null, 'p01a: 导航里有「摄像机」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(80);

    const content = doc.getElementById('content');
    const ip = key('camera.ch.0.ip');
    const brand = key('camera.ch.0.vendor');
    const stream = key('camera.ch.0.stream');
    const user = key('camera.ch.0.user');
    const pass = key('camera.ch.0.pass');
    const url = key('camera.ch.0.url');

    console.log('p02 字段是下拉/明文/无 MAC');
    chk(ip !== null && brand !== null && stream !== null && url !== null,
        'p02a: IP／品牌／码流／拉流地址四个控件都在');
    chk(brand.tagName === 'SELECT', 'p02b: 品牌是下拉（<select>），不是输入框');
    chk(stream.tagName === 'SELECT', 'p02c: 码流类型是下拉（<select>）');
    chk(brand.options.length === 6, 'p02d: 品牌 6 项＝1 个默认 + 5 个厂家，实得 ' +
        (brand ? brand.options.length : -1));
    {
        const txt = brand ? Array.from(brand.options).map((o) => o.textContent).join('/') : '';

        chk(/海康/.test(txt) && /大华/.test(txt) && /宇视/.test(txt) &&
            /华为/.test(txt) && /中维世纪/.test(txt),
            'p02e: 五个厂家都在选项里，实得 ' + txt);
        chk(/默认/.test(txt) && /海康/.test(txt.split('/')[0]),
            'p02f: 第一项是「默认：海康」（没品牌按海康）');
    }
    chk(stream.options.length === 3, 'p02g: 码流三项（默认主码流/主/子），实得 ' +
        (stream ? stream.options.length : -1));
    chk(stream.options.length > 0 && /默认：主码流/.test(stream.options[0].textContent),
        'p02g2: 第一项是「默认：主码流」（空值＝主码流），实得 ' +
        (stream && stream.options[0] ? stream.options[0].textContent : '?'));
    chk(pass.type === 'text', 'p02h: 密码是明文输入框（首版不加密），type=' +
        (pass ? pass.type : '?'));
    chk(key('camera.ch.0.mac') === null, 'p02i: 页面上没有 MAC 输入框');

    console.log('p03 进页面即按「默认海康＋IP」生成地址');
    chk(url.value === 'rtsp://192.168.1.64:554/Streaming/Channels/101',
        'p03a: 地址＝rtsp://<ip>:554/Streaming/Channels/101，实得 ' + url.value);

    console.log('p04 填账号密码 → 地址带上认证信息');
    user.value = 'admin';
    fire(user, 'input');
    pass.value = 'abc12345';
    fire(pass, 'input');
    chk(url.value === 'rtsp://admin:abc12345@192.168.1.64:554/Streaming/Channels/101',
        'p04a: 账号密码拼进 URL，实得 ' + url.value);

    console.log('p05 切品牌 → 路径跟着换');
    brand.value = 'dahua';
    fire(brand, 'change');
    chk(url.value === 'rtsp://admin:abc12345@192.168.1.64:554/cam/realmonitor?channel=1&subtype=0',
        'p05a: 大华＝/cam/realmonitor?channel=1&subtype=0，实得 ' + url.value);

    console.log('p06 切子码流 → subtype=1');
    stream.value = 'sub';
    fire(stream, 'change');
    chk(url.value === 'rtsp://admin:abc12345@192.168.1.64:554/cam/realmonitor?channel=1&subtype=1',
        'p06a: 子码流＝subtype=1，实得 ' + url.value);

    console.log('p07 手改地址后，上游字段变化不得覆盖它（现场手调的口径）');
    url.value = 'rtsp://admin:abc12345@192.168.1.64:554/custom/path';
    fire(url, 'input');
    pass.value = 'newpass99';
    fire(pass, 'input');
    chk(url.value === 'rtsp://admin:abc12345@192.168.1.64:554/custom/path',
        'p07a: 手改的地址被保留，实得 ' + url.value);

    console.log('p08 保存：只提交改动过的键');
    {
        const sec = ip.closest('.chan-sec');
        const card = sec ? sec.closest('.card') : null;
        const btn = card ? byText(card, 'button', '保存修改') : null;

        chk(btn !== null, 'p08a: 卡片上有「保存修改」');
        if (btn !== null) {
            btn.click();
            await sleep(120);
        }
        const body = applyCalls[0] || { items: {} };
        const it = body.items || {};

        chk(body.ver === 3, 'p08b: 带当前版本号 ver=3，实得 ' + body.ver);
        chk(it['camera.ch.0.vendor'] === 'dahua',
            'p08c: 品牌提交 dahua，实得 ' + JSON.stringify(it['camera.ch.0.vendor']));
        chk(it['camera.ch.0.stream'] === 'sub', 'p08d: 码流提交 sub');
        chk(it['camera.ch.0.pass'] === 'newpass99', 'p08e: 密码明文提交');
        chk(it['camera.ch.0.url'] === 'rtsp://admin:abc12345@192.168.1.64:554/custom/path',
            'p08f: 手改的地址原样提交');
        chk(it['camera.ch.0.ip'] === undefined, 'p08g: 没改过的 IP 不进提交体（脏值才发）');
        chk(it['camera.ch.0.mac'] === undefined, 'p08h: MAC 永不出现在提交体');
    }

    console.log('p09 「全部配置」页也不给 MAC 入口（skip 通配）');
    {
        /* 导航上的名字是「配置」（页面标题才叫「全部配置」），别按标题去找。 */
        const navCfg = byText(doc.getElementById('navList'), 'button', '配置') ||
            byText(doc.getElementById('navList'), 'a', '配置');

        chk(navCfg !== null, 'p09a: 导航里有「配置」入口（页面标题＝全部配置）');
        if (navCfg !== null) {
            navCfg.click();
            await sleep(120);
        }
        const c2 = doc.getElementById('content');

        chk(c2.querySelector('[data-key="camera.ch.0.mac"]') === null,
            'p09b: 原始视图里没有 MAC 输入框');
        chk(c2.querySelector('[data-key="camera.ch.0.ip"]') !== null,
            'p09c: 但同一路的 IP 仍在（通配只吃了 mac 那一段）');
    }
};

main().then(() => {
    console.log(fail === 0 ? 'ui_camera_params passed (24 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
