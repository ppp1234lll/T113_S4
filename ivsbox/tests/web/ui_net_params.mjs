/*
 * 前端行为测试：网络配置页的「本机网络与主网检测」卡片
 *
 * ============================ 为什么要有这个 ============================
 * 2026-10-26 用户指着平台「参数设置」那一组字段（网关 IP ／ 配置IP和网关 ／
 * MAC 地址 ／ 主网检测 IP 1、2 ／ 配置主网检测IP）问"在 web 界面没有显示？"。
 * 这几项在我们配置里分属 `net` 与 `probe` 两个分类，通用 configCard 只能按分类
 * 画，摆不到一起 —— 于是新加了一块专门的卡片。这次改动**全在浏览器里**，
 * `make test` / `make itest` / `make arm` 一个都碰不到，`node --check` 只管语法。
 *
 * 断言的是四类真会出错的地方：
 *   n01 卡片与五个输入框、MAC 只读值都到位；
 *   n04 **同一个键不能在页面上出现两次编辑器** —— 通用「网络参数」卡片必须把
 *       net.ip/mask/gateway 跳过，否则两个表单写同一批键，后提交的那个必然带着
 *       过期版本号撞 e=-3（"配置已被别处改动"），看起来像权限问题；
 *   n06/n07 两个按钮**各自只发自己那几个键**（多发了就会顺带改掉不想改的东西）；
 *   n08/n09 非法 IP、非连续掩码必须**在发请求之前**挡住 —— 后端
 *       `apply_saved_local_net()` 是延迟 800 ms 才跑的，那边失败只进日志，
 *       界面上已经显示"已保存"，是最难查的一种假成功。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_net_params.mjs
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

/* ---------------- fetch 桩：模拟后端的四件事 ---------------- */
const ETH_MAC = '2e:a2:9e:ef:43:43';   /* 板端 eth0 的真实 MAC（真机取证） */
let ver = 7;                            /* 不从 0 开始：能证明版本号是现取的 */
const net = { 'net.ip': '192.168.2.120', 'net.mask': '255.255.255.0',
              'net.gateway': '192.168.2.1',
              'net.server.wired.host': '47.104.250.225',
              'net.server.wired.port': 6013 };
const probe = { 'probe.interval_ms': 5000, 'probe.timeout_ms': 2000, 'probe.max_fail': 3,
                'probe.target1': '192.168.2.1', 'probe.target2': '192.168.2.2' };
const applyCalls = [];
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0) {
        return Promise.resolve(rsp({
            ok: true,
            d: {
                version: 'test',
                device: { name: null, devtype: '0a00', sysver: '1.0.0.20261026',
                          hw_model: 'FN_RZTY_PY', model: 'embedsky,tqt113-core' },
                net: {
                    link: {
                        gateway: '192.168.2.1', gateway_if: 'eth0',
                        ifaces: [
                            { name: 'eth0', present: true, up: true, carrier: true,
                              ip: '192.168.2.120', mask: '255.255.255.0', mac: ETH_MAC },
                            { name: 'usb0', present: true, up: true, carrier: true,
                              ip: '10.62.35.221', mask: '255.255.255.0', mac: '02:4b:b3:b9:eb:e5' }
                        ]
                    },
                    wan: { transport: 'up', bound: 'usb0', server: '47.104.250.225',
                           port: 6013, queue: 0, bound_ok: true, switches: 0, txn_fail: 0 }
                }
            }
        }));
    }
    if (u.indexOf('/api/config?defaults=1') === 0) {
        const items = {};
        Object.keys(net).concat(Object.keys(probe)).forEach((k) => { items[k] = { v: '', d: k }; });
        return Promise.resolve(rsp({ ok: true, d: { items } }));
    }
    if (u.indexOf('/api/config?cls=') === 0) {
        /* ⚠ 桩必须覆盖页面会打的**全部**分类：网络配置页除 net 之外还会读
         * sys（通用「网络参数」卡片是 ['net','sys'] 两个分类）。漏一个就返回
         * `{ok:true,d:{}}`，前端 `Object.keys(r.d.items)` 立刻抛异常 —— 这一条
         * 是 2026-10-22 踩过的老坑，这里一次性写全。 */
        const cls = u.split('cls=')[1].split('&')[0];
        const items = cls === 'net' ? net : (cls === 'probe' ? probe : {});
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: items } }));
    }
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        if (body.ver !== ver)   /* 真实后端：版本不符即整条拒（e=-3） */
            return Promise.resolve(rsp({ ok: false, e: -3, msg: 'version conflict' }));
        Object.keys(body.items).forEach((k) => {
            if (k.indexOf('net.') === 0) net[k] = body.items[k];
            else probe[k] = body.items[k];
        });
        ver += 1;
        return Promise.resolve(rsp({ ok: true,
                                     d: { version: ver, applied: Object.keys(body.items).length } }));
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
const q = (root, key) => root.querySelector('[data-key="' + key + '"]');
const toastText = () => {
    const t = doc.getElementById('toast');
    return t ? t.textContent : '';
};

const main = async () => {
    await sleep(50);

    console.log('n00 进入网络页');
    const nav = byText(doc.getElementById('navList'), 'button', '网络');
    chk(nav !== null, 'n00a: 导航里有「网络」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(120);

    const content = doc.getElementById('content');
    chk(content.textContent.indexOf('本机网络与主网检测') >= 0,
        'n01a: 出现「本机网络与主网检测」卡片');

    /* n01b：五个可编辑项都在，且带的是真实值 */
    const ip = q(content, 'net.ip');
    const mask = q(content, 'net.mask');
    const gw = q(content, 'net.gateway');
    const t1 = q(content, 'probe.target1');
    const t2 = q(content, 'probe.target2');

    chk(ip && ip.value === '192.168.2.120', 'n01b: 本机 IP 输入框带真实值');
    chk(mask && mask.value === '255.255.255.0', 'n01c: 掩码输入框带真实值');
    chk(gw && gw.value === '192.168.2.1', 'n01d: 网关输入框带真实值');
    chk(t1 && t1.value === '192.168.2.1' && t2 && t2.value === '192.168.2.2',
        'n01e: 主网检测 IP 1/2 带真实值');

    /* n02：MAC 只读 */
    chk(content.textContent.indexOf(ETH_MAC) >= 0, 'n02a: 显示 eth0 的真实 MAC');
    chk(content.textContent.indexOf('MAC 地址（只读）') >= 0, 'n02b: MAC 标注为只读');

    /* n03：两个按钮都在 */
    chk(byText(content, 'button', '配置 IP 和网关') !== null, 'n03a: 有「配置 IP 和网关」按钮');
    chk(byText(content, 'button', '配置主网检测 IP') !== null, 'n03b: 有「配置主网检测 IP」按钮');

    /* n04：同一个键只能有一个编辑器 —— 通用「网络参数」卡片必须跳过这三键，
     * 否则页面上两个表单写同一批键，后提交的带过期版本号必撞 e=-3。 */
    chk(content.querySelectorAll('[data-key="net.ip"]').length === 1,
        'n04a: net.ip 在整页只有 1 个输入框（通用卡片已跳过）');
    chk(content.querySelectorAll('[data-key="net.mask"]').length === 1,
        'n04b: net.mask 只有 1 个');
    chk(content.querySelectorAll('[data-key="net.gateway"]').length === 1,
        'n04c: net.gateway 只有 1 个');
    chk(content.querySelectorAll('[data-key="probe.target1"]').length === 1,
        'n04d: probe.target1 只有 1 个');
    /* 反过来也要成立：通用卡片并没有被整体砍掉（平台地址还得能改） */
    chk(q(content, 'net.server.wired.host') !== null,
        'n04e: 通用「网络参数」卡片仍在，平台地址还能改');

    /* n05：两个按钮各自只发自己那几个键 */
    console.log('n05 点「配置主网检测 IP」');
    applyCalls.length = 0;
    t1.value = '10.1.1.1';
    t1.dispatchEvent(new window.Event('input'));
    t2.value = '10.1.1.2';
    t2.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置主网检测 IP').click();
    await sleep(60);

    chk(applyCalls.length === 1, 'n05a: 只发了一次 config.apply');
    if (applyCalls.length === 1) {
        const keys = Object.keys(applyCalls[0].items).sort();
        chk(keys.length === 2 && keys[0] === 'probe.target1' && keys[1] === 'probe.target2',
            'n05b: 只发了 probe.target1/target2 两个键');
        chk(applyCalls[0].items['probe.target1'] === '10.1.1.1', 'n05c: 值就是输入框里的');
        chk(applyCalls[0].ver === 7, 'n05d: 提交的是读到的版本号 7');
    }

    console.log('n06 点「配置 IP 和网关」（版本号已涨到 8，仍能提交成功）');
    applyCalls.length = 0;
    gw.value = '192.168.2.254';
    gw.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置 IP 和网关').click();
    await sleep(60);

    chk(applyCalls.length === 1, 'n06a: 只发了一次 config.apply');
    if (applyCalls.length === 1) {
        const keys = Object.keys(applyCalls[0].items).sort();
        chk(keys.length === 3 && keys.join(',') === 'net.gateway,net.ip,net.mask',
            'n06b: 只发了 net.ip/mask/gateway 三个键');
        chk(applyCalls[0].ver === 8,
            'n06c: 版本号跟着上一次应答更新了（8），不会撞 e=-3');
        chk(applyCalls[0].items['net.gateway'] === '192.168.2.254', 'n06d: 网关值正确');
    }
    chk(toastText().indexOf('切换地址') >= 0,
        'n06e: 提示里明说"会切地址、本页会断"，不让人以为只是存了个值');

    /* n07：非法 IP 必须在发请求之前挡住 */
    console.log('n07 非法输入被前端挡住');
    applyCalls.length = 0;
    ip.value = '192.168.2.300';
    ip.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置 IP 和网关').click();
    await sleep(40);
    chk(applyCalls.length === 0, 'n07a: 非法 IP 不发请求');
    chk(toastText().indexOf('IPv4') >= 0, 'n07b: 明确提示不是合法 IPv4');

    ip.value = '192.168.2.130';
    ip.dispatchEvent(new window.Event('input'));
    mask.value = '255.0.255.0';       /* 看着像掩码、其实不连续 */
    mask.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置 IP 和网关').click();
    await sleep(40);
    chk(applyCalls.length === 0, 'n07c: 非连续掩码不发请求');
    chk(toastText().indexOf('连续') >= 0, 'n07d: 提示掩码必须是连续前缀');

    /* n08：掩码与本机 IP 必须**成对**留空或成对填写。
     * "本机 IP 留空＝不在此处指定"成立，但只填掩码不填 IP 是无意义的组合
     * （后端 `apply_saved_local_net()` 见 ip 为空就直接返回，掩码白填）——
     * 这种组合必须在前端拦掉，否则又是一次"界面上存成功、实际什么都没做"。 */
    mask.value = '';
    mask.dispatchEvent(new window.Event('input'));
    ip.value = '';
    ip.dispatchEvent(new window.Event('input'));
    gw.value = '';
    gw.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置 IP 和网关').click();
    await sleep(40);
    chk(applyCalls.length === 1 && applyCalls[0].items['net.ip'] === '' &&
        applyCalls[0].items['net.mask'] === '',
        'n08a: 三项全留空可以提交（＝沿用系统网络配置），发的是空串');

    applyCalls.length = 0;
    mask.value = '255.255.255.0';
    mask.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置 IP 和网关').click();
    await sleep(40);
    chk(applyCalls.length === 0 && toastText().indexOf('必须填本机 IP') >= 0,
        'n08b: 只填掩码不填 IP 被拦掉（否则后端拿到空 ip 直接返回，等于白填）');

    /* n09：有线网络超时时间（平台 `0xFE` 落的是 `probe.timeout_ms`）。
     * 2026-10-26 用户第二次报同一类问题：平台「参数设置」那一组里有
     * 「有线网络超时时间(ms)」，我们只把它藏在「检测与自愈」页、还叫
     * 「单次探测超时」—— 名字不对、页面也不对，照着平台找必然找不到。 */
    console.log('n09 有线网络超时时间（probe.timeout_ms）');
    const to = q(content, 'probe.timeout_ms');
    chk(to !== null, 'n09a: 「本机网络与主网检测」里有超时输入框');
    chk(to !== null && to.value === '2000', 'n09b: 带的是真实值 2000 ms');
    chk(content.textContent.indexOf('有线网络超时时间(ms)') >= 0,
        'n09c: 标签用的是平台的叫法，不是配置键描述');
    chk(byText(content, 'button', '配置有线超时') !== null, 'n09d: 有「配置有线超时」按钮');
    chk(content.querySelectorAll('[data-key="probe.timeout_ms"]').length === 1,
        'n09e: 本页只有 1 个超时输入框（运维页那份已用 skip 清单去掉，这里不重复）');

    applyCalls.length = 0;
    to.value = '100';
    to.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置有线超时').click();
    await sleep(60);
    chk(applyCalls.length === 1, 'n09f: 合法值只发一次 config.apply');
    if (applyCalls.length === 1) {
        const keys = Object.keys(applyCalls[0].items);
        chk(keys.length === 1 && keys[0] === 'probe.timeout_ms',
            'n09g: 只发 probe.timeout_ms 一个键（不多带 target/interval）');
        chk(applyCalls[0].items['probe.timeout_ms'] === 100,
            'n09h: 发的是数字 100 而不是字符串（后端按 int 收）');
    }

    /* 三个非法值都必须在**发请求之前**挡住：区间 100~60000、
     * 必须**严格小于半个探测周期**（桩里 interval=5000 ⇒ 上限 2499）、整数。
     * 「半周期」这条是 2026-10-26 语义纠正带出来的：这个键是延时门限，
     * 等回包超时＝半周期，门限摆到半周期及以上 ⇒ 时延永远够不到它。 */
    const bads = [['50', '低于下界 100'], ['5000', '不小于探测周期'],
                  ['2500', '正好等于半周期（必须严格小于）'],
                  ['2.5', '不是整数'], ['', '留空']];
    for (const [v, tag] of bads) {
        applyCalls.length = 0;
        to.value = v;
        to.dispatchEvent(new window.Event('input'));
        byText(content, 'button', '配置有线超时').click();
        await sleep(40);
        chk(applyCalls.length === 0 && toastText().length > 0,
            'n09i: ' + tag + ' 被前端拦住（不发请求、有提示）');
    }

    /* 边界内侧必须能过：2499 < 2500（半周期），前端不许一味拦死 */
    applyCalls.length = 0;
    to.value = '2499';
    to.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '配置有线超时').click();
    await sleep(60);
    chk(applyCalls.length === 1, 'n09j: 2499（半周期内侧）放行');
    if (applyCalls.length === 1) {
        chk(applyCalls[0].items['probe.timeout_ms'] === 2499,
            'n09j: 发的仍是数字 2499');
    }

    if (fail === 0) {
        console.log('ui_net_params: ' + (applyCalls.length ? 'all' : 'all') + ' checks passed');
        process.exit(0);
    }
    console.log('ui_net_params FAILED (' + fail + ' checks)');
    process.exit(1);
};

main();
