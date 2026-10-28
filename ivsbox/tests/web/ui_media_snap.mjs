/*
 * 前端行为测试：媒体页「抓拍（USB 摄像头）」卡（2026-10-28）
 *
 * ============================ 为什么要有这个 ============================
 * 这一批改动另一半在浏览器里（`web/static/app.js` 的 snapCard），
 * `make test` / `make itest` / `make arm` 全都碰不到它，`node --check` 只证明语法。
 * 而这块最容易出的三件事，恰好都是"后端对了、界面上看不出来"：
 *
 *   ① **"已受理"被当成"拍好了"**：`POST /api/snapshot` 是异步的（V4L2 出帧
 *      几百毫秒），板端真出现过"日志一条不少、文件没落盘"。界面若在 POST 回来
 *      就弹"抓拍成功"，现场会一直以为相机没问题。这里钉住：POST 之后只是
 *      "等待图片落盘"，**图片真的 load 出来**才算成功。
 *   ② **重试与放弃**：图片迟迟不出现要重试（同秒重抓会撞文件名，故 URL 要带
 *      `&r=` 破缓存），重试到头要给一句能照着查的话（"图片没有落盘"），
 *      不能无声无息地停在那里。
 *   ③ **列表与顺手提示**：进页要能看到之前抓的图（否则"抓完就丢"），并且卡片上
 *      必须写明"摄像头没有插入顺序要求"——那是用户 2026-10-28 的明确口径，
 *      不写在界面上，现场还会去重启媒体进程。
 *
 * 做法：jsdom 跑**真实的** index.html + app.js，进「媒体」页，用桩 fetch 驱动
 * 列表/抓拍，手动 dispatch 图片的 load/error 事件（jsdom 不加载图片资源，
 * 事件由测试控制 —— 这正是我们要断言的"什么时候算成功"）。
 *
 * 用法（在 ivsbox/ 目录下；jsdom 不是工程依赖，缺了会明确 SKIP）：
 *   NODE_PATH=<node_modules 所在目录> node tests/web/ui_media_snap.mjs
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
/* 目录列表形态照后端 `media_dir_list`：`{ok,d:{dir,n,items:[{n,s,t}]}}`，新的在前。 */
const LIST = {
    ok: true,
    d: {
        dir: '/snap/',
        n: 2,
        items: [
            { n: 'snap_1700000002.jpg', s: 204800, t: 1700000002 },
            { n: 'snap_1700000001.jpg', s: 102400, t: 1700000001 },
        ],
    },
};
const REC_PAGE = {
    recording: false, forced: false, held_off: false, count: 8,
    ch: [{ id: 0, url: false, want: false, on: false, failed: false }],
};

let listRsp = LIST;
let snapRsp = {
    ok: true,
    d: {
        queued: true,
        path: '/mnt/sdcard/snap/snap_1700000003.jpg',
        name: 'snap_1700000003.jpg',
        url: '/media/snap/snap_1700000003.jpg',
    },
};
let snapPosts = 0;
let listGets = 0;
const rsp = (obj) => ({ status: 200, text: () => Promise.resolve(JSON.stringify(obj)) });

window.fetch = function (u, o) {
    o = o || {};

    if (u.indexOf('/api/media/record') === 0)
        return Promise.resolve(rsp({ ok: true, d: REC_PAGE }));
    if (u.indexOf('/api/snapshot') === 0) {
        if ((o.method || 'GET').toUpperCase() !== 'POST')
            return Promise.resolve(rsp({ ok: false, e: -22, msg: 'POST required' }));
        snapPosts++;
        return Promise.resolve(rsp(snapRsp));
    }
    if (u.indexOf('/media/snap/') === 0) {
        listGets++;
        return Promise.resolve(rsp(listRsp));
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
const content = () => doc.getElementById('content');
const bigImg = () => doc.querySelector('img.snapimg');
const errEvent = () => new window.Event('error');
const loadEvent = () => new window.Event('load');

const main = async () => {
    await sleep(50);

    console.log('s01 进「媒体」页，抓拍卡在位并写明无顺序要求');
    const nav = byText(doc.getElementById('navList'), 'button', '媒体') ||
        byText(doc.getElementById('navList'), 'a', '媒体');

    chk(nav !== null, 's01a: 导航里有「媒体」入口');
    if (nav === null)
        return;
    nav.click();
    await sleep(150);

    const bShot = byText(content(), 'button', '立即抓拍');
    const bReload = byText(content(), 'button', '刷新列表');

    chk(bShot !== null, 's01b: 出现「立即抓拍」按钮');
    chk(bReload !== null, 's01c: 出现「刷新列表」按钮');
    {
        const text = content().textContent;

        /* 这句是用户口径的直接投影：不写出来，现场还会去重启媒体进程。 */
        chk(text.indexOf('没有先后顺序') >= 0, 's01d: 卡片写明摄像头插入没有先后顺序');
        chk(text.indexOf('/mnt/sdcard/snap') >= 0, 's01e: 卡片写明图片落在哪个目录');
    }
    if (bShot === null)
        return;

    console.log('s02 进页即列出现有图片，最新一张自动上大图');
    chk(listGets >= 1, 's02a: 进页读了一次 /media/snap/ 列表');
    {
        const thumbs = content().querySelectorAll('.snapthumb');
        const t0 = (thumbs.length > 0) ? thumbs[0].querySelector('img') : null;

        chk(thumbs.length === 2, 's02b: 两张缩略图，实得 ' + thumbs.length);
        /* 不按时间文本断言（那要依赖跑测试的机器时区），直接看第一张是谁 */
        chk(t0 !== null && t0.getAttribute('src').indexOf('snap_1700000002.jpg') >= 0,
            's02c: 第一张是最新的那个（前端拿 items[0] 当大图）');
    }
    {
        const im = bigImg();

        chk(im !== null && im.getAttribute('src') &&
            im.getAttribute('src').indexOf('/media/snap/snap_1700000002.jpg') === 0,
            's02d: 大图默认指向最新一张，实得 ' + (im && im.getAttribute('src')));
    }

    console.log('s03 点「立即抓拍」：POST 一次，但**不**先报成功');
    const toastEl = doc.getElementById('toast');

    bShot.click();
    await sleep(120);
    chk(snapPosts === 1, 's03a: 只发一次 POST /api/snapshot，实得 ' + snapPosts);
    chk(content().textContent.indexOf('等待图片落盘') >= 0,
        's03b: 提交后显示"等待图片落盘"，不是"成功"');
    chk(toastEl.textContent.indexOf('抓拍成功') < 0,
        's03c: 图片没到之前不许弹"抓拍成功"（板端"200 但没落盘"的教训）');
    {
        const im = bigImg();

        chk(im !== null && im.getAttribute('src') &&
            im.getAttribute('src').indexOf('/media/snap/snap_1700000003.jpg') === 0,
            's03d: 大图换成本次抓拍返回的 url');
    }

    console.log('s04 图片 load 之后才说成功，并刷新列表');
    {
        const before = listGets;
        const im = bigImg();

        im.dispatchEvent(loadEvent());
        await sleep(120);
        chk(toastEl.textContent.indexOf('抓拍成功') >= 0,
            's04a: 图片真的 load 出来才提示"抓拍成功"');
        chk(listGets > before, 's04b: 成功后重新拉一次列表（新图立刻可见）');
        chk(bShot.disabled === false, 's04c: 按钮恢复可用');
    }

    console.log('s05 后端失败：把它的原话给用户');
    snapRsp = { ok: false, e: -95, msg: 'media not wired' };
    {
        /* 切页会整页重建（go() 里 content.innerHTML=''），按钮必须重新查 */
        const b2 = byText(content(), 'button', '立即抓拍');

        b2.click();
        await sleep(120);
        chk(content().textContent.indexOf('未装配媒体链') >= 0,
            's05a: 媒体链没装配时说清是什么问题');
        chk(b2.disabled === false, 's05b: 失败后按钮恢复可用');
    }

    console.log('s06 抓到了但不在媒体根下：如实说，不装成功');
    snapRsp = {
        ok: true,
        d: { queued: true, path: '/mnt/UDISK/other/snap_1.jpg', name: 'snap_1.jpg' },
    };
    {
        const b3 = byText(content(), 'button', '立即抓拍');

        b3.click();
        await sleep(120);
        chk(content().textContent.indexOf('不在媒体根下') >= 0,
            's06a: 没有 url 时说明"页面无法显示"，并给出板端路径');
        chk(toastEl.textContent.indexOf('抓拍成功') < 0, 's06b: 这种情况也不许弹成功');
    }

    console.log('s07 图片始终出不来：重试带破缓存参数，到头给一句能照着查的话');
    snapRsp = {
        ok: true,
        d: {
            queued: true,
            path: '/mnt/sdcard/snap/snap_1700000004.jpg',
            name: 'snap_1700000004.jpg',
            url: '/media/snap/snap_1700000004.jpg',
        },
    };
    {
        const b4 = byText(content(), 'button', '立即抓拍');

        b4.click();
        await sleep(120);
        const im = bigImg();

        im.dispatchEvent(errEvent());
        await sleep(20);
        im.dispatchEvent(errEvent());
        await sleep(700);
        chk(/&r=\d+/.test(im.getAttribute('src')),
            's07a: 重试时加 &r= 破缓存（同秒重抓会撞文件名），实得 ' +
            im.getAttribute('src'));

        /* 再补 7 次（共 9 次）⇒ 越过 8 次上限，转成失败 */
        for (let i = 0; i < 7; i++) {
            im.dispatchEvent(errEvent());
            await sleep(20);
        }
        await sleep(80);
        chk(content().textContent.indexOf('图片没有落盘') >= 0,
            's07b: 重试到头给"图片没有落盘"（含排查方向），不是无声停住');
        chk(b4.disabled === false, 's07c: 放弃后按钮恢复可用');
    }
};

main().then(() => {
    console.log(fail === 0 ? 'ui_media_snap passed (18 assertions)' : fail + ' FAILED');
    process.exit(fail === 0 ? 0 : 1);
}).catch((e) => {
    console.error('HARNESS ERROR', e);
    process.exit(2);
});
