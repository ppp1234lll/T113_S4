/*
 * 前端行为测试：网络页「链路状态」里的主网编号 ↔ 槽位映射
 *
 * ============================ 为什么要有这个 ============================
 * 2026-10-29 现场：设备 eth0=192.168.1.120，有线到平台实测 100% 丢包（真的断），
 * 4G 到平台正常；界面却显示「主网1 正常 / 主网2 断开」。用户直接质问
 * "怎么主网2 是断开，主网1 却是通" —— 与上一轮"回包认领假阳性"长得一模一样，
 * 白挨了一轮怀疑。
 *
 * 真相是**编号口径对不上**：
 *   - 后端 `/api/overview` 的 `net.mn[]` 按 `iv_wan_t` 下标给：
 *     `[0]`=usb0/4G、`[1]`=eth0/有线（`iv_netmgr.h`，历史实现）；
 *   - 而"主网1"在产品口径与故障码里特指**有线主链路**（`iv_fault.h`：主网1＝eth0，
 *     4G 是备份＝主网2），`iv_agent.c:fault_feed()` 也是按角色分、不看槽位。
 *   ⇒ 前端若按槽位直显，就把"有线断"写成了"主网2 断"。
 *
 * 断言用的是**不可能撞对的取值** `[1, 2]`（4G 通、有线断）：按槽位直显会得到
 * 「主网1 正常 / 主网2 断开」，正好是错的那个；改对之后必须是
 * 「主网1（有线）断开 / 主网2（4G）正常」。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_net_mn_label.mjs
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

/* 板端现场的那一组事实：4G(mn[0])=1 正常、有线(mn[1])=2 断开 */
const NET = {
    state: 'WIRELESS_UP',
    mn: [1, 2],
    active_wan: 'wireless',
    link: {
        gateway: '192.168.1.1', gateway_if: 'eth0',
        ifaces: [
            { name: 'eth0', present: true, up: true, carrier: true,
              ip: '192.168.1.120', mask: '255.255.255.0', mac: '2e:a2:9e:ef:43:43' },
            { name: 'usb0', present: true, up: true, carrier: true,
              ip: '10.62.35.221', mask: '255.255.255.0', mac: '02:4b:b3:b9:eb:e5' }
        ]
    },
    wan: { transport: 'up', bound: 'usb0', server: '47.104.250.225', port: 6013,
           queue: 0, bound_ok: true, switches: 0, txn_fail: 0 }
};

const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u) {
    if (u.indexOf('/api/overview') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 'test', net: NET } }));
    if (u.indexOf('/api/config?') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 1, items: {} } }));
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

/* 在「链路状态」那块里按 label（.k 文本）找对应的值节点与它的着色 class */
function kvOf(root, label) {
    for (const box of root.querySelectorAll('.kv')) {
        const k = box.querySelector('.k');
        const v = box.querySelector('.v');
        if (k && v && k.textContent.indexOf(label) === 0)
            return v;
    }
    return null;
}

const main = async () => {
    await sleep(50);

    console.log('m00 进入网络页');
    const nav = byText(doc.getElementById('navList'), 'button', '网络');
    chk(nav !== null, 'm00a: 导航里有「网络」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(200);

    const content = doc.getElementById('content');

    console.log('m01 主网1 = 有线，且取的是 mn[1]');
    const m1 = kvOf(content, '主网1');
    chk(m1 !== null, 'm01a: 有「主网1」这一行');
    if (m1 === null)
        return;
    chk(m1.textContent.trim() === '断开', 'm01b: mn[1]=2 ⇒ 主网1 显示「断开」（不是「正常」）');
    chk(m1.className.indexOf('err') >= 0, 'm01c: 断开按「坏」着色（err）');

    console.log('m02 主网2 = 4G，且取的是 mn[0]');
    const m2 = kvOf(content, '主网2');
    chk(m2 !== null, 'm02a: 有「主网2」这一行');
    if (m2 === null)
        return;
    chk(m2.textContent.trim() === '正常', 'm02b: mn[0]=1 ⇒ 主网2 显示「正常」');
    chk(m2.className.indexOf('ok') >= 0, 'm02c: 正常按「好」着色（ok）');

    console.log('m03 标签本身要写明是有线还是 4G');
    chk(kvOf(content, '主网1（有线）') !== null, 'm03a: 主网1 标注「（有线）」');
    chk(kvOf(content, '主网2（4G）') !== null, 'm03b: 主网2 标注「（4G）」');

    console.log(fail === 0 ? 'ALL PASS' : (fail + ' FAILED'));
    process.exit(fail === 0 ? 0 : 1);
};

main();
