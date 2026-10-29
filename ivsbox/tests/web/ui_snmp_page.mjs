/*
 * 前端行为测试：「SNMP」独立页（2026-10-29 新建）
 *
 * ============================ 为什么要有这个 ============================
 * 用户原话：「SNMP 配置，参考单片机的 web 界面，单独新建界面进行配置」——
 * 品牌 tab（海康／大华／宇视）下每行「中文名 ＋ OID 输入框」的网格。
 * 这一页完全在浏览器里，`make test` / `make itest` / `make arm` 都碰不到，
 * `node --check` 只证明语法。四条最容易错、错了界面上还看不出来的接线：
 *
 *   ① **配置键前缀**：`camera.snmp.<品牌>.<字段>`（4 段），不是旧的
 *      `camera.snmp.oid.<字段>`。键名错了 apply 会以"未知键"静默收下
 *      （配置模块对未注册键是照存不误），采集端却永远读不到 ⇒ 界面显示成功、
 *      设备不采。这条只有比对请求体才抓得住。
 *   ② **切 tab 不能丢输入**：三张表必须同时渲染、只切 display。若用
 *      `innerHTML=''` 重画，先改海康再改大华，回头海康那格白填了 ——
 *      而且用户是"先改完再点保存"，丢的正是他刚敲的值。
 *   ③ **一个键只能有一个编辑器**：摄像机页与交换机页都已把 `camera.snmp.*` /
 *      `switch.snmp.*` 整片排除，这里若漏渲染，现场就会有两个表单写同一批键，
 *      后提交的必然带过期 `st.ver`、撞 e=-3。
 *   ④ **按路采集**：`which` 必须是 `cam0`..`cam5` / `switch`。6 路里点第 3 路
 *      却采第 1 路，界面上一点异样都没有（都是同一张表），只有取到的值不对。
 *
 * 做法：jsdom 跑**真实的** index.html + app.js —— 进「SNMP」页 → 切品牌 tab →
 * 改 OID 并保存 → 选第 3 路发起采集，断言请求体与渲染结果。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_snmp_page.mjs
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
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

/* ---------------- 桩状态 ---------------- */
let ver = 41;
const applyCalls = [];
const snmpRunCalls = [];
const snmpGets = [];

/* 出厂默认的**真实形态**：标准 MIB-II 三项三品牌都有值，私有节点只有海康有，
 * 大华/宇视留空（没有可靠依据就不编 OID —— 编错了现场拿到"有值但是错的"）。 */
const configItems = {
    'camera.snmp.enable': false,
    'camera.snmp.community': 'public',
    'camera.snmp.timeout_ms': 1500,
    'camera.snmp.retries': 1,
    'camera.snmp.hik.sysDescr': '1.3.6.1.2.1.1.1.0',
    'camera.snmp.hik.sysUpTime': '1.3.6.1.2.1.1.3.0',
    'camera.snmp.hik.sysName': '1.3.6.1.2.1.1.5.0',
    'camera.snmp.hik.deviceType': '1.3.6.1.4.1.39165.1.1.0',
    'camera.snmp.hik.softVersion': '1.3.6.1.4.1.39165.1.3.0',
    'camera.snmp.hik.serialNumber': '1.3.6.1.4.1.39165.1.5.0',
    'camera.snmp.hik.cpuPercent': '1.3.6.1.4.1.39165.1.7.0',
    'camera.snmp.hik.memUsed': '1.3.6.1.4.1.39165.1.11.0',
    'camera.snmp.hik.devStatus': '',
    'camera.snmp.dahua.sysDescr': '1.3.6.1.2.1.1.1.0',
    'camera.snmp.dahua.sysUpTime': '1.3.6.1.2.1.1.3.0',
    'camera.snmp.dahua.sysName': '1.3.6.1.2.1.1.5.0',
    'camera.snmp.dahua.deviceType': '',
    'camera.snmp.dahua.softVersion': '',
    'camera.snmp.dahua.serialNumber': '',
    'camera.snmp.dahua.cpuPercent': '',
    'camera.snmp.dahua.memUsed': '',
    'camera.snmp.dahua.devStatus': '',
    'camera.snmp.uniview.sysDescr': '1.3.6.1.2.1.1.1.0',
    'camera.snmp.uniview.sysUpTime': '1.3.6.1.2.1.1.3.0',
    'camera.snmp.uniview.sysName': '1.3.6.1.2.1.1.5.0',
    'camera.snmp.uniview.deviceType': '',
    'camera.snmp.uniview.softVersion': '',
    'camera.snmp.uniview.serialNumber': '',
    'camera.snmp.uniview.cpuPercent': '',
    'camera.snmp.uniview.memUsed': '',
    'camera.snmp.uniview.devStatus': '',
    'camera.ch.0.vendor': 'hik',
    'camera.ch.2.vendor': 'dahua',
    'switch.ip': '192.168.1.1',
    'switch.snmp.enable': true,
    'switch.snmp.port': 161,
    'switch.snmp.community': 'public',
    'switch.snmp.timeout_ms': 1500,
    'switch.snmp.retries': 1,
    'switch.snmp.oid.sysDescr': '1.3.6.1.2.1.1.1.0',
    'switch.snmp.oid.sysUpTime': '1.3.6.1.2.1.1.3.0',
    'switch.snmp.oid.ifDescr': '1.3.6.1.2.1.2.2.1.2',
    'switch.snmp.oid.ifSpeed': '1.3.6.1.2.1.2.2.1.5',
    'switch.snmp.oid.ifAdminStatus': '1.3.6.1.2.1.2.2.1.7',
    'switch.snmp.oid.ifOperStatus': '1.3.6.1.2.1.2.2.1.8',
    'switch.snmp.oid.ifInOctets': '1.3.6.1.2.1.2.2.1.10',
    'switch.snmp.oid.ifInDiscards': '1.3.6.1.2.1.2.2.1.13',
    'switch.snmp.oid.ifOutOctets': '1.3.6.1.2.1.2.2.1.16',
    'switch.snmp.oid.ifOutDiscards': '1.3.6.1.2.1.2.2.1.19',
    'switch.snmp.oid.lldpRemSysName': '1.0.8802.1.1.2.1.4.1.1.9',
    'switch.snmp.oid.lldpRemPortId': '1.0.8802.1.1.2.1.4.1.1.7'
};

window.fetch = function (u, o) {
    o = o || {};
    if (u.indexOf('/api/overview') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: 'test', cameras: { cns: [] } } }));
    if (u.indexOf('/api/config?cls=camera') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: configItems } }));
    if (u.indexOf('/api/config?cls=switch') === 0) {
        const sub = {};

        Object.keys(configItems).forEach((k) => {
            if (k.indexOf('switch.') === 0)
                sub[k] = configItems[k];
        });
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: sub } }));
    }
    if (u.indexOf('/api/config?cls=') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    if (u.indexOf('/api/config?defaults=1') === 0)
        return Promise.resolve(rsp({ ok: true, d: { version: ver, items: {} } }));
    if (u.indexOf('/api/config/apply') === 0) {
        const body = JSON.parse(o.body);

        applyCalls.push(body);
        Object.keys(body.items).forEach((k) => { configItems[k] = body.items[k]; });
        ver += 1;
        return Promise.resolve(rsp({ ok: true, d: { version: ver, applied: Object.keys(body.items).length } }));
    }
    if (u.indexOf('/api/snmp/run') === 0) {
        snmpRunCalls.push(JSON.parse(o.body));
        return Promise.resolve(rsp({ ok: true, d: { accepted: true } }));
    }
    if (u.indexOf('/api/snmp?which=') === 0) {
        snmpGets.push(u.slice(u.indexOf('which=') + 6));
        return Promise.resolve(rsp({
            ok: true, d: {
                which: u.slice(u.indexOf('which=') + 6),
                state: 2,
                slots: [{ name: 'sysDescr', oid: '.1.3.6.1.2.1.1.1.0', n_rows: 1, rows: [{ str: 'HIKVISION IPC' }] }]
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

/* 品牌面板是带 data-brand 的 div（tab 按钮也带同名属性，靠 tagName 区分） */
function panel(brand) {
    for (const e of doc.querySelectorAll('[data-brand="' + brand + '"]')) {
        if (e.tagName === 'DIV')
            return e;
    }
    return null;
}

/* 按标签文字找输入框：`fieldFor()` 把中文名写在 label 的 span 里 */
function inputByLabel(root, text) {
    for (const lab of root.querySelectorAll('label')) {
        const sp = lab.querySelector('span');

        if (sp === null || sp.textContent.trim() !== text)
            continue;
        const inp = lab.querySelector('[data-key]');

        if (inp)
            return inp;
    }
    return null;
}

function visible(e) {
    return e !== null && e.style.display !== 'none';
}

const main = async () => {
    await sleep(120);
    const nav = doc.getElementById('navList');
    const content = doc.getElementById('content');

    console.log('s01 导航里有独立「SNMP」页，且不带协议编号');
    const btn = byText(nav, 'button', 'SNMP');

    chk(btn !== null, 's01a: 导航里有「SNMP」');
    if (btn === null)
        return;
    btn.click();
    await sleep(200);

    console.log('s02 三张卡都在：摄像机 OID / 交换机 OID / 采集结果');
    chk(byText(content, 'h2', 'SNMP') !== null, 's02a: 页面标题是「SNMP」');
    chk(byText(content, 'h3', '摄像机 OID（按品牌）') !== null, 's02b: 摄像机 OID 卡');
    chk(byText(content, 'h3', '交换机 OID') !== null, 's02c: 交换机 OID 卡');
    chk(byText(content, 'h3', '采集结果') !== null, 's02d: 采集结果卡');

    console.log('s03 品牌 tab：海康默认展开，大华/宇视收起');
    chk(panel('hik') !== null && panel('dahua') !== null && panel('uniview') !== null,
        's03a: 三张品牌表都渲染出来了（切 tab 不重画，才不丢输入）');
    chk(visible(panel('hik')), 's03b: 默认显示海康');
    chk(!visible(panel('dahua')) && !visible(panel('uniview')),
        's03c: 大华/宇视默认收起');

    console.log('s04 OID 输入框的键是 camera.snmp.<品牌>.<字段>（4 段）');
    const hikName = inputByLabel(panel('hik'), '设备名称 OID');

    chk(hikName !== null, 's04a: 海康面板有「设备名称 OID」输入框');
    chk(hikName && hikName.dataset.key === 'camera.snmp.hik.sysName',
        's04b: 键名＝camera.snmp.hik.sysName，实得 ' + (hikName && hikName.dataset.key));
    chk(hikName && hikName.value === '1.3.6.1.2.1.1.5.0',
        's04c: 出厂默认值来自配置（标准 MIB-II sysName）');
    const dahuaType = inputByLabel(panel('dahua'), '设备型号 OID');

    chk(dahuaType !== null && dahuaType.dataset.key === 'camera.snmp.dahua.deviceType',
        's04d: 大华面板的「设备型号 OID」键名正确');
    chk(dahuaType && dahuaType.value === '',
        's04e: 大华私有节点出厂为空＝不采（不编 OID）');

    console.log('s05 切 tab：只切显示，已敲的值不能丢');
    byText(content, 'button', '大华').click();
    await sleep(60);
    chk(visible(panel('dahua')) && !visible(panel('hik')), 's05a: 切到大华');
    const dahuaSerial = inputByLabel(panel('dahua'), '序列号／项目编码 OID');

    dahuaSerial.value = '1.3.6.1.4.1.10004.1.1.4.0';
    dahuaSerial.dispatchEvent(new window.Event('input'));
    byText(content, 'button', '海康').click();
    await sleep(60);
    const hikName2 = inputByLabel(panel('hik'), '设备名称 OID');

    hikName2.value = '1.3.6.1.2.1.1.5.0';
    byText(content, 'button', '大华').click();
    await sleep(60);
    chk(inputByLabel(panel('dahua'), '序列号／项目编码 OID').value === '1.3.6.1.4.1.10004.1.1.4.0',
        's05b: 切回大华，刚才填的 OID 还在（未被 innerHTML 重画冲掉）');

    console.log('s06 保存：三个品牌 ＋ 采集参数一次提交');
    const saveBtns = content.querySelectorAll('button');

    for (const b of saveBtns) {
        if (b.textContent.trim() === '保存修改') {
            b.click();
            break;
        }
    }
    await sleep(120);
    chk(applyCalls.length === 1, 's06a: 只发了一次 apply（三张表共用一个版本号）');
    const items = (applyCalls[0] || {}).items || {};

    chk(items['camera.snmp.dahua.serialNumber'] === '1.3.6.1.4.1.10004.1.1.4.0',
        's06b: 提交体里是 camera.snmp.dahua.serialNumber，实得 ' + JSON.stringify(Object.keys(items)));
    chk(Object.keys(items).every((k) => k.indexOf('camera.snmp.hik.') === 0 ||
            k.indexOf('camera.snmp.dahua.') === 0 ||
            k.indexOf('camera.snmp.uniview.') === 0),
        's06c: 提交的全是 camera.snmp.<品牌>.* 键，没有残留的 camera.snmp.oid.*');

    console.log('s07 交换机 OID 一张表，12 个槽位前缀 switch.snmp.oid.');
    const swOper = inputByLabel(content, '运行状态 OID（表列）');

    chk(swOper !== null && swOper.dataset.key === 'switch.snmp.oid.ifOperStatus',
        's07a: 交换机 OID 键名＝switch.snmp.oid.ifOperStatus');
    chk(swOper && swOper.value === '1.3.6.1.2.1.2.2.1.8',
        's07b: 表列 OID 不带末尾 .0（走 GETNEXT）');

    console.log('s08 按路采集：选第 3 路就采 cam2');
    const sel = content.querySelector('select');

    chk(sel !== null && sel.options.length === 7,
        's08a: 目标下拉有 7 项（6 路摄像机 ＋ 交换机）');
    chk(sel && sel.options[0].value === 'cam0' && sel.options[6].value === 'switch',
        's08b: 命名是 cam0..cam5 / switch（与后端 target_of() 同一套）');
    snmpGets.length = 0;
    sel.value = 'cam2';
    sel.dispatchEvent(new window.Event('change'));
    await sleep(80);
    chk(snmpGets.length === 1 && snmpGets[0] === 'cam2',
        's08c: 切成第 3 路后按 which=cam2 回读，实得 ' + JSON.stringify(snmpGets));
    chk(/sysDescr/.test(content.textContent), 's08d: 结果表渲染出槽位名');
    chk(/就绪/.test(content.textContent), 's08e: 状态码 2 显示成「就绪」');

    const runBtn = byText(content, 'button', '发起采集');

    chk(runBtn !== null, 's08f: 有「发起采集」按钮');
    runBtn.click();
    await sleep(80);
    chk(snmpRunCalls.length === 1 &&
            snmpRunCalls[0].which === 'cam2',
        's08g: snmp.run 的 which 跟着下拉走（cam2），实得 ' + JSON.stringify(snmpRunCalls));
};

main().then(() => {
    console.log(fail === 0 ? 'ui_snmp_page passed (SNMP 独立页)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
