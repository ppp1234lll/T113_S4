/*
 * 工具测试：tools/tunnel-keygen.html（令牌与访问端配置生成器）
 *
 * ============================ 为什么要有这个 ============================
 * 这个工具是**运维侧唯一的换算器**：设备与 PC 两边的 secretKey 都靠它算。
 * 它算错一位数，症状是"两边都连上了 frps、但握手永远失败"—— 设备日志、
 * frps 日志、PC 日志里都没有一句"钥匙不对"。所以必须用**与设备单测同一个黄金
 * 向量**（test_tunnel t01：sn "A120F" + token "bird123" → 1239036bac71d702）
 * 把内联的 MD5 钉死，而不是"跑起来没报错"就算过。
 *
 * 同时钉三件事：
 *   - 令牌字符白名单与设备侧 `iv_tunnel_token_valid()` 一致（含引号/空格要拒）；
 *   - visitor.toml 里 serverName 必须等于 SN（设备的代理项名就是 SN）；
 *   - 输入不全时不产出半份配置（宁可空着，也别给一份能粘错的文件）。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/tool_tunnel_keygen.mjs
 */
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const toolPath = path.resolve(here, '../../tools/tunnel-keygen.html');

let JSDOM;
try {
    const req = createRequire(path.join(here, 'noop.js'));
    ({ JSDOM } = req('jsdom'));
} catch (e) {
    console.log('SKIP: jsdom not installed (npm i jsdom, or set NODE_PATH); ' + e.message);
    process.exit(0);
}

const dom = new JSDOM(fs.readFileSync(toolPath, 'utf8'), {
    runScripts: 'dangerously',
    url: 'http://127.0.0.1/'
});
const doc = dom.window.document;
const $ = (id) => doc.getElementById(id);

let fail = 0;
function chk(cond, what) {
    if (cond) console.log('  PASS ' + what);
    else { console.log('  FAIL ' + what); fail++; }
}

const main = () => {
    console.log('g01 空输入不产出配置');
    chk($('secret').textContent.indexOf('SN') >= 0, 'g01a: 初始提示"要填 SN"');
    chk($('toml').value === '', 'g01b: visitor.toml 为空（不产出半份配置）');
    chk($('bat').value === '', 'g01c: connect.bat 为空');

    console.log('g02 黄金向量（与 test_tunnel t01 同一个值）');
    $('sn').value = 'A120F';
    $('tok').value = 'bird123';
    $('host').value = 'frp.example.com';
    $('port').value = '7000';
    $('bind').value = '2022';
    $('calc').click();

    chk($('secret').textContent.trim() === '1239036bac71d702',
        'g02a: secretKey = 1239036bac71d702，实得「' + $('secret').textContent.trim() + '」')

    const toml = $('toml').value;
    chk(toml.indexOf('serverAddr = "frp.example.com"') >= 0, 'g02b: toml 带 serverAddr');
    chk(toml.indexOf('serverPort = 7000') >= 0, 'g02c: toml 带 serverPort');
    chk(toml.indexOf('auth.token = "bird123"') >= 0, 'g02d: toml 的 auth.token 就是令牌本身');
    chk(toml.indexOf('type = "stcp"') >= 0, 'g02e: 代理类型是 stcp（不是 tcp，不能把 22 暴露到公网）');
    chk(toml.indexOf('serverName = "A120F"') >= 0, 'g02f: serverName 等于 SN');
    chk(toml.indexOf('secretKey = "1239036bac71d702"') >= 0, 'g02g: toml 里的 secretKey 与结果一致');
    chk(toml.indexOf('bindPort = 2022') >= 0, 'g02h: bindPort 用页面填的值');

    const bat = $('bat').value;
    chk(bat.indexOf('frpc.exe') >= 0 && bat.indexOf('visitor.toml') >= 0,
        'g02i: connect.bat 里同时用到 frpc.exe 与 visitor.toml');
    chk(bat.indexOf('ssh -p 2022') >= 0, 'g02j: connect.bat 的提示用填的 bindPort');

    console.log('g03 设备侧要填的三项');
    chk($('devfill').textContent.indexOf('frps 地址：frp.example.com') >= 0, 'g03a: 回显地址');
    chk($('devfill').textContent.indexOf('令牌：bird123') >= 0, 'g03b: 回显令牌');

    console.log('g04 令牌字符集与设备侧同源');
    $('tok').value = 'ab"cd';
    $('calc').click();
    chk($('toml').value === '', 'g04a: 带引号的令牌被拒（不产出 toml）');
    chk($('secret').textContent.indexOf('非法字符') >= 0, 'g04b: 明确说"含非法字符"');

    $('tok').value = 'ab cd';
    $('calc').click();
    chk($('toml').value === '', 'g04c: 带空格的令牌被拒');

    $('tok').value = 'a'.repeat(64);
    $('calc').click();
    chk($('toml').value === '', 'g04d: 64 位超长被拒（设备上限 63）');

    console.log('g05 生成按钮给的是合法令牌');
    $('tok').value = '';
    $('gen').click();
    const tk = $('tok').value;

    chk(/^[0-9a-f]{32}$/.test(tk), 'g05a: 生成 32 位小写 hex');
    chk($('secret').textContent.trim().length === 16, 'g05b: 生成后 secretKey 仍是 16 位');
};

try {
    main();
} catch (e) {
    console.error('HARNESS ERROR', e);
    process.exit(2);
}
console.log(fail === 0 ? 'tool_tunnel_keygen passed (21 assertions)' : fail + ' FAILED');
process.exit(fail === 0 ? 0 : 1);
