/*
 * IVSBox 运维终端 —— 前端（M5-S5.3）
 *
 * 三条自我约束（都是计划 S5.1 点名要改掉参考项目做法的地方）：
 *   1. **模板与数据分离**：本文件与 index.html / style.css 都是独立文件，改页面
 *      不用重编固件；
 *   2. **REST/JSON**：所有数据走 /api/*，页面里没有任何 SSI 标签；
 *   3. **事件绑定**：全部 addEventListener，不用 `onchange="View.xxx()"` 这种
 *      全局函数式写法。
 *
 * 另外**表单不手写**：配置页的字段是按后端返回的键值**自动生成**的
 * （`GET /api/config?cls=xxx` 给什么键就渲染什么字段）。这样后端加一个配置键，
 * 前端不用改一行 —— 手写 100+ 个字段的做法改一个键就要动两边，正是要避免的。
 *
 * 所有动态文本一律走 textContent（绝不用 innerHTML 拼），避免现场数据里出现
 * 尖括号时把页面结构冲坏（型号串 / 设备串号是外部输入）。
 */
(function () {
  'use strict';

  /* ==================== 工具 ==================== */
  function $(sel) { return document.querySelector(sel); }

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }

  function api(path, opts) {
    var o = opts || {};
    o.credentials = 'same-origin';
    if (o.body !== undefined && typeof o.body !== 'string') {
      o.body = JSON.stringify(o.body);
      o.headers = { 'Content-Type': 'application/json' };
    }
    return fetch(path, o).then(function (r) {
      return r.text().then(function (t) {
        var j;
        try { j = JSON.parse(t); } catch (e) { j = { ok: false, e: -1, msg: '响应不是 JSON' }; }
        j._status = r.status;
        /* 仍在出厂默认密码（服务端 must_change=1）时，除改密与登出外一切 403。
         * 这一句放在 api() 里统一接管：任何一次调用都是"该改密了"的信号，
         * 不必让每个调用点各判一次（漏一个就漏一条路）。 */
        if (r.status === 403 && j.msg === 'must change password') showChpass('', true);
        /* 会话过期（8 h TTL 到点，或 Web 进程重启丢了内存里的会话表）：服务端一律回
         * 401 `{"ok":false,"e":-23,"msg":"unauthorized"}`。**必须在这里统一接管** ——
         * 各调用点只会把 ok:false 读成"这次没数据"，于是页面满屏"未获取到链路状态
         * （ivsboxd 未就绪或网络模块未装配）""读取失败：unauthorized"，看着像设备坏了，
         * 实际只是要重新登录（2026-10-29 现场：页面放了一夜，早上切到「网络」页）。
         * boot() 里那条 401 探测只覆盖"页面加载时"，覆盖不到这种"开着页面掉线"。
         * 登录接口自己的 401 是 `bad password`，提示归登录卡片，不能被这里抢走。 */
        if (r.status === 401 && path !== '/api/login')
          showLogin('登录已过期，请重新登录');
        return j;
      });
    });
  }

  var toastTimer = null;
  function toast(msg, kind) {
    var t = $('#toast');
    /* 兜底：提示条得挂在 #app 上、不能在 #content 里（那里每次切页被清空，
     * 见 index.html 的说明与 D-16）。真丢了就现造一个，宁可样式粗糙也不要
     * 让这里的异常把调用方后面的语句（版本号更新/表单重载）一起带走。 */
    if (!t) {
      t = el('div', 'toast hidden');
      t.id = 'toast';
      t.setAttribute('role', 'status');
      ($('#app') || document.body).appendChild(t);
    }
    t.textContent = msg;
    t.className = 'toast ' + (kind || '');
    if (toastTimer) clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = 'toast hidden'; }, 3500);
  }

  function fmtUptime(ms) {
    var s = Math.floor((ms || 0) / 1000);
    var d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d > 0) return d + ' 天 ' + h + ' 小时';
    if (h > 0) return h + ' 小时 ' + m + ' 分';
    return m + ' 分 ' + (s % 60) + ' 秒';
  }

  function kv(k, v, cls) {
    var box = el('div', 'kv');
    box.appendChild(el('div', 'k', k));
    box.appendChild(el('div', 'v' + (cls ? ' ' + cls : ''), v));
    return box;
  }

  var CNS_NAME = { 0: '无IP', 1: '正常', 2: '故障', 4: '延时', 5: '丢包' };
  /* 主网状态（上报帧 `MN=` 每路一个数）：码值与后端/平台一致，别自己加名字。
   * 3/4 是 2026-10-26 补上的：网线被拔报 3、平均时延超门限报 4（「延时」），
   * 原来只有 0/1/2 ⇒ 这两种情况界面上显示「—」，现场看不出是拔线还是慢。 */
  var MN_NAME = { 0: '未判定', 1: '正常', 2: '断开', 3: '网线断开', 4: '延时' };
  var SNMP_STATE = { 0: '空闲', 1: '采集中', 2: '就绪' };
  /* 设备类型（上报帧头 `DEVTYPE=` 的码值）→ 可读名。
   * 码值由后端 `sys.overview.device.devtype` 给：**出厂定死、没有配置入口**
   * （2026-10-26 用户拍板）。表只有这一张，加类型只改这里；
   * 表里没有的码值就只显示十六进制、**不编一个名字** —— 显示错的类型名比显示码值
   * 更误导现场。键用小写，与后端 `%04x` 的写法一致。 */
  var DEVTYPE_NAME = { '0a00': '嵌入式T113' };

  /* ==================== 状态 ==================== */
  var PAGES = [
    { id: 'overview', name: '总览' },
    { id: 'net', name: '网络' },
    { id: 'cell', name: '4G 模块' },
    { id: 'gps', name: '定位' },
    { id: 'board', name: '控制板' },
    { id: 'camera', name: '摄像机' },
    { id: 'devices', name: '设备档案' },
    { id: 'switch', name: '交换机' },
    { id: 'media', name: '媒体' },
    { id: 'config', name: '配置' },
    { id: 'maint', name: '运维' }
  ];

  var st = { page: 'overview', ver: 0, version: '' };

  /* ==================== 登录 / 首次改密（M10） ==================== */
  /* 口令规则与提示语：**与服务端是同一句话**（`web/src/iv_web.h` 的
   * `IV_WEB_PASS_HINT_TEXT`，源头是参考实现 index.shtml:976 的 alert）。
   * 前端这份只负责"输入时就告诉用户"，真正的闸门在服务端 —— 前端校验挡不住 curl。 */
  var PASS_RULE = /^(?=.*[a-z])(?=.*[A-Z])(?=.*\d)[\s\S]{8,12}$/;
  var PASS_HINT = '请输入正确的密码格式：\n密码长度:8-12\n密码由数字+大写英文字母+小写英文字母组成';

  /* 刚登录成功后拿到的口令：只在**用户主动改密**时用来填 `old` 字段。
   * 强制改密不要它（服务端那条路径不校验旧口令），页面刷新后也拿不到 —— 都不影响。 */
  var lastPass = '';
  var chpassForced = false;

  function showLogin(msg) {
    $('#chpass').className = 'login hidden';
    $('#app').className = 'app hidden';
    $('#login').className = 'login';
    $('#loginMsg').textContent = msg || '';
  }

  /* forced=true：出厂默认密码，必须先改；false：用户自己点了"改密"（多要一个原密码）。 */
  function showChpass(msg, forced) {
    chpassForced = !!forced;
    $('#login').className = 'login hidden';
    $('#app').className = 'app hidden';
    $('#chpass').className = 'login';
    $('#chpassTitle').textContent = chpassForced ? '请修改登录密码' : '修改登录密码';
    $('#chpassWhy').textContent = chpassForced
      ? '设备仍在使用出厂默认密码，必须先修改才能继续使用。' : '';
    $('#oldWrap').className      = chpassForced ? 'hidden' : '';
    $('#chpassCancel').className = chpassForced ? 'ghost hidden' : 'ghost';
    $('#chpassMsg').textContent  = msg || '';
  }

  function showApp() {
    $('#login').className = 'login hidden';
    $('#chpass').className = 'login hidden';
    $('#app').className = 'app';
    buildNav();
    go(st.page);
  }

  function tryLogin(user, pass) {
    return api('/api/login', { method: 'POST', body: { user: user, pass: pass } })
      .then(function (r) {
        if (r.ok && r.d && r.d.token) {
          lastPass = pass;
          $('#loginMsg').textContent = '';
          /* 服务端说还得改密 —— 直接切改密卡片，不给进主界面 */
          if (r.d.must_change) { showChpass('', true); return true; }
          showApp();
          return true;
        }
        showLogin(r.msg === 'bad password' ? '账号或密码不正确' : (r.msg || '登录失败'));
        return false;
      }).catch(function () { showLogin('无法连接服务'); return false; });
  }

  function changePass(oldPw, newPw, newPw2) {
    if (newPw !== newPw2) { $('#chpassMsg').textContent = '两次输入的新密码不一致'; return; }
    if (!PASS_RULE.test(newPw)) { $('#chpassMsg').textContent = PASS_HINT; return; }
    api('/api/passwd', { method: 'POST', body: { old: oldPw, new: newPw } })
      .then(function (r) {
        if (r.ok) { lastPass = ''; toast('密码已修改', 'ok'); showApp(); return; }
        if (r._status === 403) return;   /* 仍在待改密态：由 api() 接管切卡片 */
        $('#chpassMsg').textContent = (r._status === 401) ? '原密码不正确'
                                     : (r.msg || '修改失败');
      }).catch(function () { $('#chpassMsg').textContent = '无法连接服务'; });
  }

  /* ==================== 4G 模块页 ==================== */

  /* 注册状态（3GPP 27.007 的 +CEREG stat）。后端已经给了一份中文 text，
   * 这里只留表给"text 缺失"时兜底 —— 两处都写全等于给自己制造漂移。 */
  var CELL_REG_FALLBACK = {
    0: '未注册，未在搜网', 1: '已注册（本地网）', 2: '未注册，正在搜网',
    3: '注册被拒绝', 4: '未知原因', 5: '已注册（漫游）'
  };

  function pageCell() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '4G 模块'));

    /* 结论卡放最前：现场打开这一页想知道的是"到底能不能用、卡在哪一层"，
     * 而不是先看 20 个字段自己去推。 */
    var top = el('div', 'card');
    top.appendChild(el('h3', null, '可用性'));
    var note = el('p', 'hint', '读取中…');
    top.appendChild(note);
    var gTop = el('div', 'grid');
    top.appendChild(gTop);
    var bar = el('div', 'actions');
    var btn = el('button', 'ghost', '刷新');
    btn.type = 'button';
    bar.appendChild(btn);
    top.appendChild(bar);
    root.appendChild(top);

    function mkCard(title) {
      var c = el('div', 'card');
      c.appendChild(el('h3', null, title));
      var g = el('div', 'grid');
      c.appendChild(g);
      root.appendChild(c);
      return g;
    }
    var gModule = mkCard('模组信息');
    var gSim    = mkCard('SIM 卡');
    var gSig    = mkCard('信号');
    var gReg    = mkCard('网络注册');
    var gCell   = mkCard('服务小区');
    var gData   = mkCard('数据连接');
    var gLink   = mkCard('AT 链路');

    /* null / 空串一律显示 "—"：后端把"读不到"统一给 null，
     * 前端不该把 null 显示成 0 或 "undefined"。 */
    function D(v, unit) {
      if (v === null || v === undefined || v === '') return '—';
      return (unit ? (v + unit) : ('' + v));
    }
    function B(v) {
      if (v === true) return '是';
      if (v === false) return '否';
      return '—';
    }
    function mnc2(v) {
      if (v === null || v === undefined) return '—';
      return (v < 10) ? ('0' + v) : ('' + v);
    }

    function load() {
      api('/api/cell').then(function (r) {
        gTop.innerHTML = '';
        if (!r.ok) {
          note.textContent = '';
          var why = (r.msg === 'agent unavailable' || r.msg === 'cellular unavailable')
            ? 'ivsboxd 未运行，或本机未装配 4G 查询模块'
            : (r.msg || '读取失败');
          gTop.appendChild(kv('状态', why, 'err'));
          return;
        }
        var d = r.d || {};
        var lk = d.link || {}, mod = d.module || {}, sim = d.sim || {}
          , sig = d.signal || {}, reg = d.reg || {}, cl = d.cell || {}, dt = d.data || {};

        /* ---- 结论 ---- */
        note.textContent = d.usable
          ? '4G 可用：SIM 就绪、已注册、PS 已附着、已取得 IP。'
          : ('4G 不可用：' + (d.reason || '原因未知（可能 AT 口未打开）'));

        gTop.appendChild(kv('可用性', d.usable ? '可用' : '不可用',
                            d.usable ? 'ok' : 'err'));
        gTop.appendChild(kv('当前断点', D(d.reason, ''), d.usable ? 'ok' : 'warn'));
        gTop.appendChild(kv('最近成功', (lk.age_ms === null || lk.age_ms === undefined)
                            ? '从未' : (Math.round(lk.age_ms / 1000) + ' 秒前')));

        /* ---- 模组 ---- */
        gModule.innerHTML = '';
        gModule.appendChild(kv('型号', D(mod.model)));
        gModule.appendChild(kv('固件', D(mod.firmware)));
        gModule.appendChild(kv('轮询周期', '30 秒'));
        gModule.appendChild(kv('完成轮数', D(lk.ok_rounds)));
        gModule.appendChild(kv('失败轮数', D(lk.fail_rounds),
                              (lk.fail_rounds > 0) ? 'warn' : null));

        /* ---- SIM ---- */
        gSim.innerHTML = '';
        gSim.appendChild(kv('是否插入', B(sim.present),
                            sim.present === false ? 'err' : null));
        gSim.appendChild(kv('卡状态', D(sim.state),
                            (sim.state === 'READY') ? 'ok' : 'warn'));
        gSim.appendChild(kv('ICCID', D(sim.iccid)));
        gSim.appendChild(kv('IMSI', D(sim.imsi)));

        /* ---- 信号 ---- */
        gSig.innerHTML = '';
        /* CSQ 0..31 是模块原始值，dBm 才是现场熟悉的量；两个都给，
         * 因为对表时两边都可能被引用（AT 手册看 CSQ、现场看 dBm）。 */
        gSig.appendChild(kv('信号格', (sig.level === null || sig.level === undefined)
                            ? '—' : (sig.level + ' / 5')));
        gSig.appendChild(kv('CSQ', D(sig.csq, '')));
        gSig.appendChild(kv('接收电平', D(sig.dbm, ' dBm')));
        gSig.appendChild(kv('RSRP', D(sig.rsrp, ' dBm')));
        gSig.appendChild(kv('RSRQ', D(sig.rsrq, ' dB')));
        /* SINR 是模块原始值：不同固件单位不一致，不替它换算 */
        gSig.appendChild(kv('SINR（原始）', D(sig.sinr)));

        /* ---- 注册 ---- */
        gReg.innerHTML = '';
        var regText = reg.text || CELL_REG_FALLBACK[reg.status] || '—';
        gReg.appendChild(kv('注册状态', regText,
                            (reg.status === 1 || reg.status === 5) ? 'ok' : 'warn'));
        gReg.appendChild(kv('运营商', D(reg.operator)));
        gReg.appendChild(kv('制式', D(reg.act)));
        gReg.appendChild(kv('MCC / MNC',
                            (reg.mcc === null || reg.mcc === undefined)
                              ? '—' : (reg.mcc + ' / ' + mnc2(reg.mnc))));

        /* ---- 小区 ---- */
        gCell.innerHTML = '';
        gCell.appendChild(kv('TAC', D(cl.tac)));
        gCell.appendChild(kv('Cell ID', D(cl.cell_id)));
        gCell.appendChild(kv('PCI', D(cl.pci)));
        gCell.appendChild(kv('频段', D(cl.band)));
        gCell.appendChild(kv('EARFCN', D(cl.earfcn)));

        /* ---- 数据连接 ---- */
        gData.innerHTML = '';
        gData.appendChild(kv('PS 附着', B(dt.attached),
                             dt.attached === false ? 'warn' : null));
        /* active 是三态：true=已建立 / false=明确没有 / null=没读到（ECM 常见） */
        gData.appendChild(kv('PDP 上下文',
                             dt.active === true ? '已建立'
                                                : (dt.active === false ? '无' : '—'),
                             dt.active === true ? 'ok'
                                                : (dt.active === false ? 'warn' : null)));
        gData.appendChild(kv('本机 IP', D(dt.ip)));
        gData.appendChild(kv('APN', D(dt.apn)));
        gData.appendChild(kv('DNS', (dt.dns1 || dt.dns2)
                            ? (D(dt.dns1) + ' / ' + D(dt.dns2)) : '—'));

        /* ---- AT 链路 ---- */
        gLink.innerHTML = '';
        gLink.appendChild(kv('设备节点', D(lk.path)));
        gLink.appendChild(kv('打开状态', lk.open ? '已打开' : '未打开',
                             lk.open ? 'ok' : 'err'));
        gLink.appendChild(kv('收字节', D(lk.rx_bytes)));
        gLink.appendChild(kv('命令超时', D(lk.cmd_timeouts),
                             (lk.cmd_timeouts > 0) ? 'warn' : null));
        gLink.appendChild(kv('最近错误码', (lk.at_err === 0) ? '无' : D(lk.at_err),
                             (lk.at_err === 0) ? null : 'warn'));
      }).catch(function () {
        note.textContent = '';
        gTop.innerHTML = '';
        gTop.appendChild(kv('状态', '无法连接服务', 'err'));
      });
    }

    btn.addEventListener('click', load);
    load();
    /* 10 s 刷新：后端 30 s 才查一轮 AT，这里只是读快照，开销可忽略。
     * 切页时由 go() 统一 clearInterval。 */
    st.timer = setInterval(load, 10000);
    return root;
  }

  /* ==================== 导航 ==================== */
  function buildNav() {
    var list = $('#navList');
    list.innerHTML = '';
    PAGES.forEach(function (p) {
      var b = el('button', null, p.name);
      b.type = 'button';
      b.dataset.page = p.id;
      b.addEventListener('click', function () { go(p.id); });
      list.appendChild(b);
    });
  }

  function markNav() {
    var bs = $('#navList').querySelectorAll('button');
    for (var i = 0; i < bs.length; i++)
      bs[i].className = (bs[i].dataset.page === st.page) ? 'active' : '';
  }

  function go(page) {
    st.page = page;
    markNav();
    /* 定位页会挂一个自动刷新定时器；切页必须先收掉，否则后台一直打接口 */
    if (st.timer) { clearInterval(st.timer); st.timer = null; }
    var c = $('#content');
    c.innerHTML = '';
    var builder = {
      overview: pageOverview, net: pageNet, cell: pageCell, gps: pageGps,
      board: pageBoard,
      camera: pageCamera,
      devices: pageDevices, switch: pageSwitch, media: pageMedia,
      config: pageConfig, maint: pageMaint
    }[page] || pageOverview;
    c.appendChild(builder());
  }

  /* ==================== 配置表单（通用） ==================== */

  /* 按点分键的前缀分组，让表单有层次（例：net.wan.fail_n → "net.wan" 组）。 */
  function groupOf(key) {
    var i = key.lastIndexOf('.');
    return (i < 0) ? key : key.slice(0, i);
  }

  /* ==================== 中文标签（键说明） ==================== */
  /* 来源：后端 `GET /api/config?defaults=1` 里每个键的 `d` 字段
   * ＝ 默认值表的 desc，**与配置文件里的行注释同源**。
   * 前端**不另抄一张"键→中文"对照表**：抄一份就多一处会过期的地方 ——
   * 后端加一个键、前端忘了改，界面又露出 `net.server.wired.host` 这种裸键名。
   * 只拉一次并缓存；拿不到就回落成键名（宁可露裸键名，也不要空白标签）。 */
  var descMap = null;
  var descLoading = null;

  function loadDescs() {
    if (descMap !== null) return Promise.resolve(descMap);
    if (descLoading !== null) return descLoading;
    descLoading = api('/api/config?defaults=1').then(function (r) {
      descMap = {};
      if (r && r.ok && r.d && r.d.items) {
        Object.keys(r.d.items).forEach(function (k) {
          var e = r.d.items[k];
          if (e && e.d) descMap[k] = e.d;
        });
      }
      return descMap;
    }).catch(function () { descMap = {}; return descMap; });
    return descLoading;
  }

  /* 分组标题的中文。**只覆盖已知分组，未知分组自动回落成裸键名** ——
   * 后端加了新分组也不会漏渲染，只是那一组标题暂时是英文键。 */
  var GROUP_NAME = {
    sys: '基础参数', net: '本机网络', probe: '检测探活', camera: '摄像机',
    switch: '交换机', elec: '电阈值', netthr: '网络阈值', sensor: '传感器阈值',
    'sys.tunnel': '远程运维通道',
    'sys.device': '设备命名与型号',
    'net.server': '平台服务器', 'net.server.wired': '有线平台服务器',
    'net.server.wireless': '无线平台服务器', 'net.server.upgrade': '升级服务器',
    'net.server.log': '日志服务器',
    'camera.ch': '摄像机通道', 'camera.detect': '摄像机检测',
    'camera.snmp': '摄像机 SNMP', 'camera.snmp.oid': '摄像机 OID 槽位',
    'switch.snmp': '交换机 SNMP', 'switch.snmp.oid': '交换机 OID 槽位'
  };

  /* 单个配置项的小编辑框。`note` 是可选的只读状态小字（例如「主网检测 IP」后面
   * 跟一句"当前状态：通"），默认不传就不显示 —— 保持老调用点行为不变。 */
  function fieldFor(key, val, desc, note) {
    var lab = el('label', 'field');
    var isBool = (typeof val === 'boolean');

    if (isBool) lab.className = 'field chk';
    lab.appendChild(el('span', null, desc || key));
    lab.title = key;   /* 悬停显示配置键：运维查配置/改 JSON 时要能对上号 */

    var inp;
    if (isBool) {
      inp = document.createElement('input');
      inp.type = 'checkbox';
      inp.checked = !!val;
      lab.appendChild(inp);
      lab.appendChild(el('span', null, val ? '已启用' : '已停用'));
    } else if (typeof val === 'number') {
      inp = document.createElement('input');
      inp.type = 'number';
      inp.value = String(val);
      inp.step = (Number.isInteger(val) ? '1' : 'any');
      lab.appendChild(inp);
    } else {
      inp = document.createElement('input');
      inp.type = 'text';
      inp.value = (val === null || val === undefined) ? '' : String(val);
      lab.appendChild(inp);
    }

    inp.dataset.key = key;
    inp.dataset.kind = isBool ? 'boolean' : (typeof val === 'number' ? 'number' : 'string');
    inp.dataset.orig = JSON.stringify(isBool ? !!val : val);

    function check() {
      if (inp.dataset.kind === 'boolean')
        inp.className = (inp.checked === JSON.parse(inp.dataset.orig)) ? '' : 'dirty';
      else if (inp.dataset.kind === 'number')
        inp.className = (Number(inp.value) === JSON.parse(inp.dataset.orig)) ? '' : 'dirty';
      else
        inp.className = (inp.value === JSON.parse(inp.dataset.orig)) ? '' : 'dirty';
    }
    inp.addEventListener('input', check);
    inp.addEventListener('change', check);
    if (note) {
      var nt = el('span', 'hint', note);
      nt.dataset.note = key;   /* 用例/排查时按配置键定位这句小字 */
      lab.appendChild(nt);
    }
    return lab;
  }

  /* 下拉版 fieldFor（2026-10-27）。契约与 fieldFor **完全一致** ——
   * `data-key` / `data-kind` / `data-orig` 挂在 select 元素上，
   * 于是 `collectChanges()` / `applyChanges()` 一个字都不用改就能收它，
   * 不必为下拉再写一套"收集＋脏标记"的逻辑（两套必然漂移）。
   *
   * `opts` = `[[值, 显示文字], ...]`；空值项（值为空串）表示"未设置/用默认"。
   * `kind='boolean'` ＝**布尔用两选一下拉**（启用/停用）：提交体里必须仍是布尔，
   * 因为配置模块按默认值表的类型校验，"true" 这种字符串会被判 type mismatch、
   * 整个分类回滚 —— 所以布尔有自己的取值/比较分支，不跟字符串那条混。
   *
   * **值不在选项里时补一个临时项**：存量配置可能存着表里没列的值（老版本手填、
   * 或平台写进来的），而 `sel.value = cur` 遇到不存在的值会静默落到第一项 ⇒
   * 用户只是点了一下保存，就把别人的配置改掉了，界面上还看不出来。
   */
  function fieldSelect(key, val, opts, desc, kind) {
    var lab = el('label', 'field');
    lab.appendChild(el('span', null, desc || key));
    lab.title = key;

    var isBool = (kind === 'boolean');
    var sel = document.createElement('select');
    var cur = isBool ? (val === true ? 'true' : 'false')
                     : ((val === null || val === undefined) ? '' : String(val));
    var i, hit = false;

    for (i = 0; i < opts.length; i++) {
      var o = document.createElement('option');
      o.value = String(opts[i][0]);
      o.textContent = String(opts[i][1]);
      sel.appendChild(o);
      if (String(opts[i][0]) === cur) hit = true;
    }
    if (!hit) {
      var ox = document.createElement('option');
      ox.value = cur;
      ox.textContent = (cur === '') ? '（未设置）' : (cur + '（表中无此项）');
      sel.appendChild(ox);
    }
    sel.value = cur;
    sel.dataset.key = key;
    sel.dataset.kind = isBool ? 'boolean' : 'string';
    sel.dataset.orig = isBool ? JSON.stringify(val === true) : JSON.stringify(cur);
    sel.addEventListener('change', function () {
      var now = isBool ? (sel.value === 'true') : sel.value;
      sel.className = (JSON.stringify(now) === sel.dataset.orig) ? '' : 'dirty';
    });

    lab.appendChild(sel);
    return lab;
  }

  function collectChanges(scope) {
    var items = {};
    var inputs = scope.querySelectorAll('[data-key]');
    for (var i = 0; i < inputs.length; i++) {
      var inp = inputs[i], kind = inp.dataset.kind, cur, orig = JSON.parse(inp.dataset.orig);
      /* 布尔既可能是勾选框（`fieldFor`）也可能是两选一下拉（`fieldSelect` 的
       * `kind='boolean'`）——2026-10-27「是否录像」改下拉后两条路都得认，
       * 否则下拉那一路的 `inp.checked` 是 undefined ⇒ 提交体里出现 `false`，
       * 用户选了"启用"却被静默改成停用。 */
      if (kind === 'boolean')
        cur = (inp.tagName === 'SELECT') ? (inp.value === 'true') : inp.checked;
      else if (kind === 'number') cur = Number(inp.value);
      else cur = inp.value;
      if (JSON.stringify(cur) !== inp.dataset.orig && (kind === 'boolean' || orig !== cur))
        items[inp.dataset.key] = cur;
    }
    return items;
  }

  function applyChanges(scope, after) {
    var items = collectChanges(scope);
    var keys = Object.keys(items);
    if (keys.length === 0) { toast('没有改动', 'ok'); return; }
    api('/api/config/apply', { method: 'POST', body: { ver: st.ver, items: items } })
      .then(function (r) {
        if (r.ok) {
          toast('已保存 ' + r.d.applied + ' 项（版本 ' + r.d.version + '）', 'ok');
          st.ver = r.d.version;
          if (after) after();
        } else if (r.e === -3) {
          toast('配置已被别处改动，请刷新后重试', 'err');
        } else {
          toast('保存失败：' + (r.msg || r.e), 'err');
        }
      })
      .catch(function () { toast('请求失败', 'err'); });
  }

  /* 拉一个分类的配置，渲染成「分组 → 表单」。classes 可以是数组。
   * onReady(items, reload)：数据就绪后的回调。渲染循环只画"已经存在的键"，
   * 于是「按需建槽」那类配置（camera.ch.N.*）在没有任何键时无输入框可画，
   * 得靠调用方自己补一个入口 —— 回调就是为了把 items 与 reload 交出去。
   *
   * skip：**本卡片不画**的键名数组。存在的唯一理由是"同一个键只能有一个编辑器"：
   * `net.ip`/`net.mask`/`net.gateway` 与 `probe.target1/2` 现在有专门的
   * 「本机网络」卡片（对齐平台那一组字段），若通用卡片再画一遍，页面上就有两个
   * 表单写同一批键 —— 后提交的那个必然带着过期的 `st.ver`，撞 e=-3。
   *
   * **跳过名单支持通配**（2026-10-27）：`camera.ch.*` 跳过整片前缀、
   * `camera.ch.*.mac` 只跳那一种字段。摄像机通道是"8 路 × 8 键"，
   * 逐条列 64 个键名既写不下、也会漏（加一路就多 8 个）。
   * 两种写法必须分清：**尾部 `*` ＝ 整片前缀**（`camera.ch.*` 吃掉
   * `camera.ch.0.ip`），**中间的 `*` 只吃一段**（不跨点号，`camera.ch.*.mac`
   * 匹配 `camera.ch.5.mac` 而不会顺手吃掉 `camera.ch.5.record`）。
   */
  function skipMatch(skip, key) {
    for (var i = 0; i < skip.length; i++) {
      var pat = skip[i];
      if (pat === key) return true;
      if (pat.indexOf('*') < 0) continue;
      if (pat.charAt(pat.length - 1) === '*') {
        if (key.indexOf(pat.slice(0, -1)) === 0) return true;
        continue;
      }
      var re = '^' + pat.split('*').map(function (part) {
        /* `.` 必须转义：否则 `camera.ch.0.mac` 里的 `.` 会匹配任意字符，
         * 把 `cameraXchY0Zmac` 这种不存在的键也判进来。 */
        return part.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
      }).join('[^.]*') + '$';
      if (new RegExp(re).test(key)) return true;
    }
    return false;
  }

  function configCard(title, classes, note, onReady, skip) {
    var skipList = skip || [];
    var scope = el('div');
    var card = el('div', 'card');
    card.appendChild(el('h3', null, title));
    if (note) card.appendChild(el('p', 'hint', note));

    var body = el('div');
    var bar = el('div', 'actions');
    var save = el('button', null, '保存修改');
    save.type = 'button';
    var reload = el('button', 'ghost', '重新载入');
    reload.type = 'button';
    bar.appendChild(save); bar.appendChild(reload);

    function load() {
      body.innerHTML = '';
      var reqs = classes.map(function (c) { return api('/api/config?cls=' + c); });
      reqs.push(loadDescs());          /* 中文标签与配置一起等，避免先闪一屏裸键名 */
      Promise.all(reqs).then(function (rs) {
        var descs = rs.pop() || {};
        var items = {}, ver = 0, bad = null;
        rs.forEach(function (r) {
          if (!r.ok) { bad = r.msg || '配置不可用'; return; }
          ver = r.d.version;
          Object.keys(r.d.items).forEach(function (k) { items[k] = r.d.items[k]; });
        });
        if (bad) { body.appendChild(el('p', 'msg', bad)); return; }
        st.ver = ver;
        var groups = {};
        Object.keys(items).sort().forEach(function (k) {
          if (skipMatch(skipList, k)) return;   /* 有专门卡片的键，这里不画第二遍 */
          var g = groupOf(k);
          (groups[g] = groups[g] || []).push(k);
        });
        Object.keys(groups).sort().forEach(function (g) {
          var sec = el('div', 'card');
          sec.style.marginBottom = '12px';
          var h = el('h3', null, GROUP_NAME[g] || g);
          h.title = g;
          sec.appendChild(h);
          var grid = el('div', 'grid');
          groups[g].forEach(function (k) { grid.appendChild(fieldFor(k, items[k], descs[k])); });
          sec.appendChild(grid);
          body.appendChild(sec);
        });
        body.appendChild(el('p', 'hint', '共 ' + Object.keys(items).length + ' 项，版本 ' + ver));
        if (onReady) onReady(items, load);
      });
    }

    save.addEventListener('click', function () { applyChanges(body, load); });
    reload.addEventListener('click', load);
    card.appendChild(body);
    card.appendChild(bar);
    scope.appendChild(card);
    load();
    return scope;
  }

  /* ==================== 页面 ==================== */
  /* ==================== 总览：设备基础信息 ====================
   * 总览页只答一个问题："这台设备是谁"。
   *   - 网络状态 / 主网1 / 主网2  → 「网络」页（链路判定）＋「当前生效」块（内核事实）
   *   - 摄像机六路 CNS           → 「摄像机」页
   *   - 采集板镜像（电压/温湿度/箱门/功率） → 「控制板」页
   * 之前这些全堆在总览页，结果四类语义不同的数据挤在一起，而且采集板的 float 是
   * 按 %.17g 直接打出来的（"40.40999984741211"）—— 控制板页早已用 %.2f 修过，
   * 总览这块是另一条路径，搬走更省事。
   *
   * 后端约定：取不到的值是 JSON null（**不是空串**）。null 显示"—"；
   * "空串"表示"读到了，但值是空的"，两者不能混。 */
  /* 检测方式的名字表挪到摄像机页（`CD_MODE_OPT`，带下拉选项）。原先是
   * `CD_MODE` 常量只用于总览页那行只读文字，2026-10-22 摄像机页接管后已无引用。 */

  var NET_STATE = {
    WIRELESS_UP: '无线已连接',
    WIRELESS_DEGRADED: '无线信号差',
    SWITCH_TO_WIRED: '正在切到有线',
    WIRED_UP: '有线已连接',
    SWITCH_TO_WIRELESS: '正在切到无线',
    BOTH_DOWN: '双链路断开'
  };

  /* 构建时没注入 git 元数据，iv_version 会拼出 "1.0.0+nogit"（D-11 之前的版本串
   * 是 "1.0.0+gunknown"，两种都要认 —— 板上跑着的旧产物还没换）。
   * 原样显示容易被当成版本号写错，这里点明是开发构建。 */
  function fmtVer(v) {
    if (!v) return '—';
    if (v.indexOf('+nogit') >= 0) return v.replace('+nogit', '') + '（开发构建）';
    if (v.indexOf('+gunknown') >= 0) return v.replace('+gunknown', '') + '（开发构建）';
    return v;
  }

  function fmtWall(sec) {
    if (!sec) return '—';
    var d = new Date(Number(sec) * 1000);
    if (!isFinite(d.getTime())) return '—';
    return d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()) +
      ' ' + pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
  }

  function pageOverview() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '总览'));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '设备信息'));
    var hint = el('p', 'hint', '读取中…');
    card.appendChild(hint);
    var grid = el('div', 'grid');
    card.appendChild(grid);

    /* ---- 设备名称（`sys.device.name`，2026-10-22 用户要求可 Web 配）----
     * 走配置事务（`config.apply`），**不是**身份分区：名称是"这一台叫什么"，
     * 现场随时会改；身份分区存的是 devid/MAC 那种一改平台就认成另一台设备的东西。
     * 版本号每次**现取**：本页没有 configCard 帮忙缓存，拿旧版本号提交必然撞 e=-3
     * （「配置已被别处改动」）—— 那种失败最容易被误读成"没权限"。 */
    var nameBar = el('div', 'actions');
    var nameLab = el('label', 'field');
    nameLab.appendChild(el('span', null, '设备名称'));
    var inName = document.createElement('input');
    inName.type = 'text';
    inName.maxLength = 64;
    inName.size = 30;
    inName.placeholder = '如「3 号换热站」；留空＝未命名';
    nameLab.appendChild(inName);
    nameBar.appendChild(nameLab);
    var nameSave = el('button', null, '保存名称');
    nameSave.type = 'button';
    nameBar.appendChild(nameSave);
    card.appendChild(nameBar);
    var nameMsg = el('p', 'msg', '');
    card.appendChild(nameMsg);

    root.appendChild(card);

    /* ---- 设备身份写入（用户 2026-10-17 要求：设备 ID / MAC 可 Web 配置）----
     * 三件事必须在界面上说清楚，否则现场一定误判：
     *   ① 改的是「身份 MAC」，**不是网口 MAC**（后者由天嵌 ini 的
     *      mac_mode=fixed + mac= 决定，改这里 ip link 不会变）；
     *   ② 只填一项则另一项保持不变（后端按字段合并，不是整份覆盖）；
     *   ③ 写入后**需重启**才对远程通道生效 —— 隧道只在启动时读一次身份。 */
    var idCard = el('div', 'card');
    idCard.appendChild(el('h3', null, '设备身份（写入工厂分区）'));
    idCard.appendChild(el('p', 'hint',
      '设备 ID 与设备 MAC 存于工厂身份分区。只填一项则另一项保持不变。'
      + '这里改的是「身份 MAC」，不是网口 MAC（网口 MAC 由系统网络配置的 mac_mode / mac 决定）。'
      + '写入后需重启才影响远程通道 —— 隧道只在启动时读一次身份。'));
    var idGrid = el('div', 'grid');

    function textField(label, ph) {
      var lab = el('label', 'field');

      lab.appendChild(el('span', null, label));
      var inp = document.createElement('input');
      inp.type = 'text';
      inp.placeholder = ph;
      lab.appendChild(inp);
      idGrid.appendChild(lab);
      return inp;
    }

    var inId = textField('设备 ID（1~6 位 HEX）', '如 1 / 000001 / A120F');
    var inMac = textField('设备 MAC', 'aa:bb:cc:dd:ee:ff');
    idCard.appendChild(idGrid);

    var idBar = el('div', 'actions');
    var idSave = el('button', null, '写入身份');
    idSave.type = 'button';
    idBar.appendChild(idSave);
    idCard.appendChild(idBar);
    var idMsg = el('p', 'msg', '');
    idCard.appendChild(idMsg);
    root.appendChild(idCard);

    function loadInfo() {
      grid.innerHTML = '';
      api('/api/overview').then(function (r) {
        if (!r.ok) { hint.textContent = r.msg || '总览读取失败'; return; }
        var d = r.d || {};
        var dv = d.device || {};
        st.version = d.version || '';
        $('#verLabel').textContent = st.version;
        hint.textContent = '设备身份与版本。设备 SN 未做工厂写入时取默认值 1（橙色即为默认值）。'
          + '实时状态请看「网络」「摄像机」「控制板」各页。';
        grid.appendChild(kv('设备名称', dv.name || '未命名',
          dv.name ? null : 'warn'));
        /* 设备类型：**出厂定死**，没有配置入口（2026-10-26 用户拍板）。
         * 后端给码值 `devtype`（4 位小写十六进制），名字取自本文件的
         * `DEVTYPE_NAME` —— 后端只给标识符、中文全在前端，是这个工程的既有分工。 */
        var dtHex = String(dv.devtype || '').toLowerCase();
        var dtName = DEVTYPE_NAME[dtHex];
        grid.appendChild(kv('设备类型',
          dtHex ? (dtHex.toUpperCase() + (dtName ? '（' + dtName + '）' : '')) : '—'));
        /* 软件版本用**平台口径** `sysver`（`1.0.0.<构建日期>`）；工程串 `version`
         * 带 `+g<短哈希>`，只在后端没给 sysver 时兜底（旧后端兼容）。 */
        grid.appendChild(kv('软件版本', dv.sysver || fmtVer(dv.version || d.version)));
        /* 「硬件型号」与「板卡型号」是两件事，别合并：
         *   硬件型号 ＝ 配置键 `sys.device.model`，也就是平台 0xE3 应答的 `mod`，
         *              随箱体变体走、配置页可改；
         *   板卡型号 ＝ 设备树 compatible 串，由板卡决定、现场改不了。
         * 改前这两者只显示了后者却标成"硬件型号"，与平台报上去的 mod 对不上号。 */
        grid.appendChild(kv('硬件型号', dv.hw_model || '—'));
        grid.appendChild(kv('板卡型号', dv.model || '—'));
        grid.appendChild(kv('主控芯片', dv.soc || '—'));
        /* 设备 SN 缺省值＝1（2026-10-17 用户定）：工厂身份分区没写入时后端就回 "1"。
         * 但来源要标出来（`sn_source`），否则售后会把默认值当成产线写入的真 SN ——
         * 默认值用告警色，一眼能看出"这个 1 不是产线写的"。 */
        grid.appendChild(kv('设备 SN', dv.sn || '—',
          dv.sn_source === 'default' ? 'warn' : null));
        grid.appendChild(kv('身份 MAC', dv.id_mac || '—'));
        grid.appendChild(kv('网口 MAC（eth0）', dv.eth0_mac || '—'));
        grid.appendChild(kv('主机名', dv.hostname || '—'));
        grid.appendChild(kv('内核版本',
          dv.kernel ? (dv.kernel + (dv.machine ? '（' + dv.machine + '）' : '')) : '—'));
        grid.appendChild(kv('系统时间', fmtWall(dv.wall_sec)));
        grid.appendChild(kv('运行时长', fmtUptime(d.uptime_ms)));
        /* 输入框预填当前值。默认值（"1"）也一起填进去，否则用户得自己猜现在是什么。 */
        if (!inId.dataset.touched) inId.value = dv.sn || '';
        if (!inMac.dataset.touched) inMac.value = dv.id_mac || '';
        if (!inName.dataset.touched) inName.value = dv.name || '';
      });
    }

    inId.addEventListener('input', function () { inId.dataset.touched = '1'; });
    inMac.addEventListener('input', function () { inMac.dataset.touched = '1'; });
    inName.addEventListener('input', function () { inName.dataset.touched = '1'; });

    nameSave.addEventListener('click', function () {
      var v = (inName.value || '').replace(/^\s+|\s+$/g, '');
      nameSave.disabled = true;
      nameMsg.textContent = '保存中…';
      api('/api/config?cls=sys').then(function (r) {
        if (!r.ok) {
          nameSave.disabled = false;
          nameMsg.textContent = '读取配置失败：' + (r.msg || ('错误 ' + r.e));
          return null;
        }
        var items = {};
        items['sys.device.name'] = v;
        return api('/api/config/apply',
          { method: 'POST', body: { ver: r.d.version, items: items } });
      }).then(function (r) {
        if (!r) return;
        nameSave.disabled = false;
        if (r.ok) {
          toast('设备名称已保存（版本 ' + r.d.version + '）', 'ok');
          nameMsg.textContent = '';
          inName.dataset.touched = '';   /* 交回回读：以设备里存的为准 */
          loadInfo();
        } else if (r.e === -3) {
          nameMsg.textContent = '配置已被别处改动，请刷新后重试';
        } else {
          nameMsg.textContent = '保存失败：' + (r.msg || ('错误 ' + r.e));
        }
      }).catch(function () {
        nameSave.disabled = false;
        nameMsg.textContent = '无法连接服务';
      });
    });

    idSave.addEventListener('click', function () {
      var dev = inId.value.trim();
      var mac = inMac.value.trim();

      if (!dev && !mac) { idMsg.textContent = '设备 ID 与 MAC 至少填一项'; return; }
      if (!window.confirm('确认写入设备身份？\n'
          + '设备 ID：' + (dev || '（不改）') + '\n'
          + '设备 MAC：' + (mac || '（不改）') + '\n'
          + '写入工厂身份分区，之后需重启才对远程通道生效。')) return;
      idSave.disabled = true;
      idMsg.textContent = '写入中…';
      api('/api/device/identity', { method: 'POST', body: { devid: dev, mac: mac } })
        .then(function (r) {
          idSave.disabled = false;
          if (!r.ok) {
            idMsg.textContent = '写入失败：' + (r.msg || ('错误 ' + r.e));
            return;
          }
          var rd = r.d || {};
          idMsg.textContent = '已写入设备 ID ' + rd.devid + '、MAC ' + (rd.mac || '（未设置）')
            + '。重启后对远程通道生效。';
          inId.dataset.touched = '';
          inMac.dataset.touched = '';
          inId.value = rd.devid || '';
          inMac.value = rd.mac || '';
          loadInfo(); /* 回读：区里的真实值以读回来的为准，不以本次输入为准 */
        })
        .catch(function () {
          idSave.disabled = false;
          idMsg.textContent = '无法连接服务';
        });
    });

    loadInfo();
    return root;
  }

  /* ==================== 本机网络与主网检测（对齐平台那一组） ====================
   * 平台「参数设置」里这几项是摆在一起的：网关 IP ／ 「配置IP和网关」／ MAC 地址 ／
   * 主网检测 IP 1、2 ／ 「配置主网检测IP」。我们这边它们在配置里分属 `net` 与
   * `probe` 两个分类，通用 configCard 只能按分类画，摆不到一起（用户 2026-10-26
   * 要求"收成一组"）。所以单独一块卡片，两个按钮各提交自己那几个键。
   *
   * 生效口径（两个按钮不一样，别混）：
   *   - **本机地址**：`config.apply` 返回后，后端 NET 分支会**延迟 800 ms** 才调
   *     `apply_saved_local_net()` 去改网口 —— 要等 HTTP 响应先回到浏览器，否则
   *     浏览器看到的是"请求失败"，而配置其实已经存进去了。所以按下之后本页会断，
   *     必须按新地址重开。
   *   - **主网检测**：`probe` 分类本来就有订阅者，`config.apply` 一返回就已热更，
   *     不断链、本页能继续用。
   *
   * 前端先做一遍格式校验（IPv4 合法性 ＋ 掩码连续性）：后端 `apply_saved_local_net()`
   * 在 800 ms 后才跑，校验失败只会进日志，界面上却已经显示"已保存"—— 那是最难查的
   * 一种"假成功"。能在前端挡住的一定挡住。 */
  function isIPv4(s) {
    var p = String(s).split('.');
    if (p.length !== 4) return false;
    for (var i = 0; i < 4; i++) {
      if (!/^\d{1,3}$/.test(p[i])) return false;
      if (Number(p[i]) > 255) return false;
    }
    return true;
  }

  /* 掩码必须"前面全 1、后面全 0"。`255.0.255.0` 这种看着像掩码的写法后端会拒
   * （`iv_netcfg_prefix()` 返回 -1），前端也一并挡掉。 */
  function maskOk(s) {
    if (!isIPv4(s)) return false;
    var v = 0, seen0 = false, i;
    var p = String(s).split('.');
    for (i = 0; i < 4; i++) v = (v << 8) | Number(p[i]);
    if (v === 0) return false;
    for (i = 31; i >= 0; i--) {
      if (((v >>> i) & 1) === 0) seen0 = true;
      else if (seen0) return false;
    }
    return true;
  }

  function netParamsCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '本机网络与主网检测'));
    var hint = el('p', 'hint',
      '「配置 IP 和网关」会立刻改本机地址：保存后约 1 秒网口切换，本页连接会断，' +
      '请按新地址重新打开。三项留空＝不在此处指定（沿用系统网络配置）。' +
      '「配置有线超时」改的是**延时门限**：设备每轮 ping 测一次平均时延，超过这个数' +
      '就在「网络状态」里把对应主网标成「延时」（链路照用、不切换、不重启），' +
      '没超过就是「正常」。它不断链、不重启。');
    card.appendChild(hint);

    var grid = el('div', 'grid');
    card.appendChild(grid);

    var bar = el('div', 'actions');
    var btnNet = el('button', null, '配置 IP 和网关');
    var btnProbe = el('button', null, '配置主网检测 IP');
    var btnTimeout = el('button', null, '配置有线超时');
    var btnReload = el('button', 'ghost', '重新载入');
    btnNet.type = 'button'; btnProbe.type = 'button'; btnTimeout.type = 'button';
    btnReload.type = 'button';
    bar.appendChild(btnNet); bar.appendChild(btnProbe); bar.appendChild(btnTimeout);
    bar.appendChild(btnReload);
    card.appendChild(bar);

    var ver = 0;
    var descs = {};
    /* 最近一次载入的配置值（net + probe 合并）。点「配置有线超时」时要拿
     * `probe.interval_ms` 做"超时必须小于探测周期"的前置校验，而那个键自己
     * 没有输入框 —— 只能从这份快照里取。 */
    var cur = {};

    function load() {
      grid.innerHTML = '';
      var reqs = [api('/api/config?cls=net'), api('/api/config?cls=probe'),
                  api('/api/overview')];
      reqs.push(loadDescs());
      Promise.all(reqs).then(function (rs) {
        var d = rs.pop() || {};
        var rn = rs[0] || {}, rp = rs[1] || {}, ro = rs[2] || {};
        if (!rn.ok || !rp.ok) {
          grid.appendChild(kv('配置', '读取失败：' + ((rn.msg || rp.msg) || '不可用'), 'err'));
          return;
        }
        descs = d;
        ver = rn.d.version;
        var it = {};
        Object.keys(rn.d.items).forEach(function (k) { it[k] = rn.d.items[k]; });
        Object.keys(rp.d.items).forEach(function (k) { it[k] = rp.d.items[k]; });

        grid.appendChild(fieldFor('net.ip', it['net.ip'], '本机 IP'));
        grid.appendChild(fieldFor('net.mask', it['net.mask'], '子网掩码'));
        grid.appendChild(fieldFor('net.gateway', it['net.gateway'], '网关 IP'));
        /* 两个「主网检测 IP」的连通状态（只显示）：后端为这两项单独建了探测项
         * （出口绑有线），与"有线/4G 能不能到平台"**不同源** —— 前者说明"本地网络
         * 通不通"，后者说明"能不能到平台"。两者一起看才分得清是谁的问题。
         * 数据来自 `/api/overview` 的 `net.targets`：0 未知 / 1 通 / 2 不通 / 3 延时。 */
        var tgs = (ro.ok && ro.d && ro.d.net && ro.d.net.targets) ? ro.d.net.targets : [];
        var TGT_TXT = { 0: '未检测', 1: '通', 2: '不通', 3: '延时' };
        var tgtNote = function (i) {
          var t = tgs[i];
          if (!t || !t.ip) return '';
          var s = TGT_TXT[t.state];
          return '当前状态：' + (s === undefined ? t.state : s);
        };
        grid.appendChild(fieldFor('probe.target1', it['probe.target1'], '主网检测 IP 1',
                                  tgtNote(0)));
        grid.appendChild(fieldFor('probe.target2', it['probe.target2'], '主网检测 IP 2',
                                  tgtNote(1)));
        /* 平台「参数设置」页把这一项摆在"配置IP和网关 / 主网检测IP"同一组里，
         * 名字是「有线网络超时时间(ms)」。我们原来只在「检测与自愈」页用配置键的
         * 中文描述「单次探测超时」显示 —— 名字对不上、又换了一页，现场照着平台
         * 找必然找不到（2026-10-26 用户第二次报同一类问题）。这里标成平台的叫法。
         * ⚠ 它的语义是**延时门限**（平均时延超过它就标「延时」），不是等回包超时；
         * 平台那个名字沿用不改（契约），卡片顶部 hint 里写清了用途。 */
        grid.appendChild(fieldFor('probe.timeout_ms', it['probe.timeout_ms'],
                                  '有线网络超时时间(ms)'));
        cur = it;

        /* MAC 只读：它是 eth0 的硬件事实（`/api/overview` → net.link），不是配置项。
         * 平台那边同一个位置也是只读显示的，标成可编辑只会误导。 */
        var mac = '—';
        var lk = (ro.ok && ro.d && ro.d.net) ? ro.d.net.link : null;
        if (lk && lk.ifaces) {
          lk.ifaces.forEach(function (f) { if (f.name === 'eth0' && f.mac) mac = f.mac; });
        }
        var macBox = kv('MAC 地址（只读）', mac);
        grid.appendChild(macBox);
      }).catch(function () {
        grid.appendChild(kv('配置', '请求失败', 'err'));
      });
    }

    function pick(key) {
      var inp = grid.querySelector('[data-key="' + key + '"]');
      return inp ? String(inp.value).replace(/^\s+|\s+$/g, '') : '';
    }

    /* 真正提交（含版本号与失败/断链两套提示）。抽出来是因为下面还有一个
     * **不走 IPv4 校验**的提交者：探测超时是数字，`submit()` 那套地址校验对它不适用。 */
    function applyItems(items, okMsg) {
      api('/api/config/apply', { method: 'POST', body: { ver: ver, items: items } })
        .then(function (r) {
          if (!r.ok) {
            toast(r.e === -3 ? '配置已被别处改动，请刷新后重试'
                             : '保存失败：' + (r.msg || r.e), 'err');
            return;
          }
          ver = r.d.version;
          toast(okMsg, 'ok');
        })
        .catch(function () {
          /* 走到这里多半是**地址已经切了**（连接被拆），不是保存失败。 */
          toast('连接已断开 —— 地址应该已经切换，请按新 IP 重新打开本页', 'warn');
        });
    }

    function submit(keys, okMsg) {
      var items = {};
      var bad = '';
      var i;

      for (i = 0; i < keys.length; i++) {
        var k = keys[i];
        var v = pick(k);
        if (v !== '' && !isIPv4(v)) { bad = (descs[k] || k) + ' 不是合法的 IPv4 地址'; break; }
        items[k] = v;
      }
      if (!bad && keys.indexOf('net.mask') >= 0) {
        var ip = items['net.ip'], mask = items['net.mask'];
        if (ip === '' && mask !== '') bad = '填了子网掩码就必须填本机 IP';
        if (ip !== '' && mask === '') bad = '填了本机 IP 就必须填子网掩码';
        if (!bad && mask !== '' && !maskOk(mask)) bad = '子网掩码必须是连续的前缀（如 255.255.255.0）';
      }
      if (bad) { toast(bad, 'err'); return; }

      applyItems(items, okMsg);
    }

    btnNet.addEventListener('click', function () {
      submit(['net.ip', 'net.mask', 'net.gateway'],
             '已保存，正在切换地址…（约 1 秒后本页会断，请按新 IP 重开）');
    });
    btnProbe.addEventListener('click', function () {
      submit(['probe.target1', 'probe.target2'], '已保存并生效（探测目标不重启、不断链）');
    });
    /* 有线网络超时（`0xFE` 落的就是这个键）：整数毫秒，区间与配置默认值表一致
     * `100..60000`，且必须**小于探测周期的一半** —— 后端的跨键校验就是这么判的。
     * 为什么是"一半"：等回包超时由周期推导（`interval/2`），时延超过它就不是
     * "延时"而是"丢包"了；门限摆在半周期及以上 ⇒ 这个键配了也永远不触发。
     * 三样都在发请求前挡住：后端拒绝只会变成一句"保存失败：…"，而现场更常见的
     * 理解是"我明明点了保存"（与上面 n07/n08 同一类假成功）。 */
    btnTimeout.addEventListener('click', function () {
      var raw = pick('probe.timeout_ms');
      var n = Number(raw);
      var itv = Number(cur['probe.interval_ms']);
      var why = '';
      var items = {};

      if (raw === '' || !/^\d+$/.test(raw)) {
        why = '有线网络超时时间（延时门限）必须是整数毫秒';
      } else if (n < 100 || n > 60000) {
        why = '有线网络超时时间（延时门限）必须在 100~60000 毫秒之间';
      } else if (isFinite(itv) && itv > 0 && n * 2 >= itv) {
        why = '延时门限必须小于探测周期的一半（当前周期 ' + itv +
              ' ms，等回包超时就是 ' + Math.floor(itv / 2) + ' ms）';
      }
      if (why) { toast(why, 'err'); return; }

      items['probe.timeout_ms'] = n;
      applyItems(items, '已保存并生效（延时门限即时生效，不断链）');
    });
    btnReload.addEventListener('click', load);

    load();
    return card;
  }

  function pageNet() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '网络配置'));

    /* 「当前生效」：配置里的 net.ip / net.mask / net.gateway 留空＝"不在此指定，
     * 交给系统网络配置决定"（本产品实际由天嵌 EmbedSkyProgress.ini 生效）。
     * 光看三个空框根本看不出设备现在是什么地址，所以单列一块**实际值**（只读，
     * 数据来自内核：/api/overview 的 d.net.link）。 */
    /* 链路判定（网络状态／活动出口／主网1／主网2）与"当前生效地址"是两件事：
     * 前者是 netmgr 的判定结果（带迟滞，会在 MAIN1/MAIN2 之间来回），后者是内核
     * 当前事实。这两块原先都在总览页，现统一收到本页 —— 判断网络问题不用来回切。 */
    var linkCard = el('div', 'card');
    linkCard.appendChild(el('h3', null, '链路状态（只读）'));
    var lHint = el('p', 'hint', '读取中…');
    linkCard.appendChild(lHint);
    var lGrid = el('div', 'grid');
    linkCard.appendChild(lGrid);
    root.appendChild(linkCard);

    /* 「平台通信」：回答"现在到底哪个网口在跟平台通信"。判定（netmgr 的活动出口）
     * 与事实（传输层实际绑定的网卡）分开显示 —— 两者不一致时报文其实是从另一条
     * 链路出去的，只看活动出口看不出来（2026-10-19 就出过这事）。 */
    var wanCard = el('div', 'card');
    wanCard.appendChild(el('h3', null, '平台通信（只读）'));
    var wHint = el('p', 'hint', '读取中…');
    wanCard.appendChild(wHint);
    var wGrid = el('div', 'grid');
    wanCard.appendChild(wGrid);
    root.appendChild(wanCard);

    var live = el('div', 'card');
    live.appendChild(el('h3', null, '当前生效（只读）'));
    var hint = el('p', 'hint', '读取中…');
    live.appendChild(hint);
    var grid = el('div', 'grid');
    live.appendChild(grid);
    root.appendChild(live);
    api('/api/overview').then(function (r) {
      var ok = (r && r.ok && r.d);
      var nt = ok ? (r.d.net || null) : null;
      var lk = nt ? nt.link : null;

      /* —— 链路状态：判定结果（迟滞后的），与下面的内核事实分开说 —— */
      if (!nt) {
        lHint.textContent = '未获取到链路状态（ivsboxd 未就绪或网络模块未装配）。';
      } else {
        lHint.textContent = '这是链路判定的结果（带迟滞）。当前内核里的真实地址见下一块。';
        lGrid.appendChild(kv('网络状态',
          NET_STATE[nt.state] || (nt.state === 'n/a' ? '未装配' : nt.state) || '—',
          nt.state === 'BOTH_DOWN' ? 'err'
            : (nt.state === 'WIRED_UP' || nt.state === 'WIRELESS_UP' ? 'ok' : 'warn')));
        /* 活动出口挪到下面「平台通信」块（那边带网口名，比 wired/wireless 更有用） */
        if (nt.mn && nt.mn.length) {
          /* 3（网线断开）按"坏"着色；4（延时）是"可用但慢" ⇒ 只到 warn 一档，
           * 与后端口径一致：延时严重不切链路、不触发自愈重启。 */
          var mnCls = function (s) {
            if (s === 2 || s === 3) return 'err';
            if (s === 1) return 'ok';
            return 'warn';
          };
          /* **槽位顺序 ≠ 链路名**（2026-10-29 现场栽过）。
           * 后端 `net.mn` 是按 `iv_wan_t` 下标给的：`[0]`=usb0/4G、`[1]`=eth0/有线。
           * 按槽位直显会把"有线断了"写成"4G 断开"。
           *
           * 标签**只写介质名**（有线 / 4G），不写"主网1／主网2"（2026-10-29 用户
           * 口径）：那套编号只在平台协议与故障码里出现，且"主网1＝有线、主网2＝4G"
           * 与这里的槽位顺序相反 —— 界面上摆出来，现场必然把两个数字对反（已经反过
           * 一次）。看介质名不会有歧义，要查协议码去故障码/日志里查。 */
          lGrid.appendChild(kv('有线', MN_NAME[nt.mn[1]] || '—', mnCls(nt.mn[1])));
          lGrid.appendChild(kv('4G', MN_NAME[nt.mn[0]] || '—', mnCls(nt.mn[0])));
        }
      }

      /* —— 平台通信：当前通信网口（判定）＋ 传输层实际绑定（事实） —— */
      var wv = nt ? nt.wan : null;
      if (!nt || nt.state === 'n/a' || !wv) {
        wHint.textContent = '未获取到平台通信状态（ivsboxd 未就绪或网络模块未装配）。';
      } else {
        var KIND = { wireless: '无线（4G）', wired: '有线' };
        var TRN = {
          up: '已连接', connecting: '建连中…',
          down: '未连接（等待重试）', closed: '未启动', unknown: '未知'
        };
        wHint.textContent = '「通信网口」是链路判定结果；「实际绑定」是传输层真正使用的网卡，' +
          '两者不一致说明报文其实从另一条链路出去。';
        wGrid.appendChild(kv('当前通信网口',
          (wv.ifname || '未配置网口名') + (KIND[wv.kind] ? '　' + KIND[wv.kind] : ''),
          wv.ifname ? 'ok' : 'warn'));
        wGrid.appendChild(kv('平台连接', TRN[wv.transport] || wv.transport || '—',
          wv.transport === 'up' ? 'ok' : 'warn'));
        wGrid.appendChild(kv('传输层实际绑定',
          wv.bound_ok ? (wv.bound || '—')
            : (wv.bound ? wv.bound + '（与通信网口不一致）' : '未绑定，走内核默认路由'),
          wv.bound_ok ? 'ok' : (wv.bound ? 'err' : 'warn')));
        wGrid.appendChild(kv('平台地址',
          wv.server ? wv.server + ':' + wv.port : '未配置', wv.server ? null : 'warn'));
        wGrid.appendChild(kv('待发送队列', (wv.queue || 0) + ' 条',
          wv.queue > 0 ? 'warn' : 'ok'));
        wGrid.appendChild(kv('累计出口切换',
          (wv.switches || 0) + ' 次' + (wv.txn_fail ? '　失败 ' + wv.txn_fail + ' 次' : ''),
          wv.txn_fail ? 'warn' : null));
        if (wv.bind_fail) {
          wGrid.appendChild(kv('绑定失败',
            wv.bind_fail + ' 次（errno=' + wv.bind_errno + '）', 'err'));
        }
      }

      hint.textContent = '以下是设备当前的真实地址（取自内核），与下方配置项里填的值无关。';
      if (!lk) { grid.appendChild(kv('状态', '未获取到网口信息', 'warn')); return; }
      var NAMES = { eth0: '有线网口 eth0', usb0: '无线网口 usb0' };
      (lk.ifaces || []).forEach(function (f) {
        var nm = NAMES[f.name] || f.name;
        var st, cls;
        /* 四态、不是两态：**网口开着但没载波**（拔线 / 4G 掉线）与"网口没启用"
         * 是两回事，前者 IPv4 地址往往还残留着（拨号遗迹），最容易被误读成
         * "还连着"。所以 up 与 carrier 必须分开说。 */
        if (!f.present) { st = '未检测到'; cls = 'warn'; }
        else if (!f.up) { st = '已关闭'; cls = 'warn'; }
        else if (!f.carrier) { st = '已启用，无载波（链路断开）'; cls = 'err'; }
        else if (!f.ip) { st = '有载波，无 IPv4 地址'; cls = 'warn'; }
        else { st = '已连接'; cls = 'ok'; }
        grid.appendChild(kv(nm, st + (f.ip ? '　' + f.ip : ''), cls));
        grid.appendChild(kv(nm + ' · 子网掩码', f.mask || '—'));
        grid.appendChild(kv(nm + ' · MAC', f.mac || '—'));
      });
      /* 默认网关只认"出口有载波"的那条。被忽略的路由单列一行 ——
       * 现场「ip route 里明明有 default，设备却说没有」就查这一行。 */
      grid.appendChild(kv('默认网关',
        (lk.gateway || '无') + (lk.gateway_if ? '（' + lk.gateway_if + '）' : ''),
        lk.gateway ? null : (lk.gateway_ignored ? 'err' : 'warn')));
      if (lk.gateway_ignored) {
        grid.appendChild(kv('已忽略的默认路由', lk.gateway_ignored, 'err'));
      }
    });

    /* 本机地址与检测目标单独一组（对齐平台「参数设置」那一组字段）。顺序按现场
     * 排查动线：先看真实地址（上两块只读），再改它，最后才是其余网络配置。 */
    root.appendChild(netParamsCard());

    root.appendChild(configCard('网络参数（net.server.* / sys.*）', ['net', 'sys'],
      '含内外网平台地址、无线/有线网口与传输模式。' +
      '本机 IP / 掩码 / 网关在上方「本机网络与主网检测」里改，这里不再重复。',
      null, ['net.ip', 'net.mask', 'net.gateway']));
    return root;
  }

  /* ==================== 定位（GPS / NMEA，M2-S2.5） ==================== */
  /* 数据来源 `GET /api/gps` → ivsboxd 的 `gps.status`。后端把"未知"一律给
   * JSON null（不是 0）—— 经纬度 0 是几内亚湾里的真实位置，拿 0 顶替"未知"
   * 会让人以为设备在赤道。所以下面每个格式化函数都必须**先判 null**。 */
  var GPS_QUALITY = {
    0: '无效（未定位）', 1: 'GPS 定位', 2: '差分定位（DGPS）',
    3: 'PPS 定位', 4: 'RTK 固定', 5: 'RTK 浮动'
  };

  function fmtNum(v, unit, digits) {
    if (v === null || v === undefined) return '—';
    var n = Number(v);
    if (!isFinite(n)) return '—';
    return n.toFixed(digits === undefined ? 1 : digits) + (unit || '');
  }

  function fmtCoord(v, pos, neg) {
    if (v === null || v === undefined) return '—';
    var n = Number(v);
    if (!isFinite(n)) return '—';
    return Math.abs(n).toFixed(6) + '° ' + (n >= 0 ? pos : neg);
  }

  function fmtAge(ms) {
    if (ms === null || ms === undefined) return '从未收到语句';
    var s = Math.floor(Number(ms) / 1000);
    if (!isFinite(s)) return '—';
    if (s < 1) return '刚刚';
    if (s < 60) return s + ' 秒前';
    if (s < 3600) return Math.floor(s / 60) + ' 分 ' + (s % 60) + ' 秒前';
    return Math.floor(s / 3600) + ' 小时前';
  }

  function pad2(n) { return (n < 10 ? '0' : '') + n; }

  function pageGps() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '定位（GPS）'));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '定位状态'));
    var note = el('p', 'hint', '读取中…');
    card.appendChild(note);
    var grid = el('div', 'grid');
    card.appendChild(grid);
    var bar = el('div', 'actions');
    var btn = el('button', 'ghost', '刷新');
    btn.type = 'button';
    bar.appendChild(btn);
    card.appendChild(bar);
    root.appendChild(card);

    var card2 = el('div', 'card');
    card2.appendChild(el('h3', null, '链路与解析统计'));
    var grid2 = el('div', 'grid');
    card2.appendChild(grid2);
    root.appendChild(card2);

    function load() {
      api('/api/gps').then(function (r) {
        grid.innerHTML = '';
        grid2.innerHTML = '';
        if (!r.ok) {
          note.textContent = '';
          var why = (r.msg === 'agent unavailable' || r.msg === 'gps unavailable')
            ? 'ivsboxd 未运行，或本机未装配 GPS'
            : (r.msg || '读取失败');
          grid.appendChild(kv('状态', why, 'err'));
          return;
        }
        var d = r.d || {}, f = d.fix || {}, utc = d.utc || {};
        var lk = d.link || {}, s = d.stats || {};

        note.textContent = f.valid
          ? '已定位（' + (GPS_QUALITY[f.fix_quality] || ('质量 ' + f.fix_quality)) + '）'
          : '尚未定位 —— 已收到卫星信号但还没有定位解算结果，请检查天线位置与天空视野。';

        /* 坐标只在 valid 时显示：定位丢失后**经纬度仍保留上一次有效值**
         * （`on_gga` 的刻意行为，现场要能看"最后定在哪"），但把旧坐标当成
         * 当前位置显示就是错的 —— 一律以 valid 为闸门。 */
        grid.appendChild(kv('定位状态', f.valid ? '有效' : '未定位', f.valid ? 'ok' : 'warn'));
        grid.appendChild(kv('经度', f.valid ? fmtCoord(f.lon, 'E', 'W') : '—'));
        grid.appendChild(kv('纬度', f.valid ? fmtCoord(f.lat, 'N', 'S') : '—'));
        grid.appendChild(kv('海拔', f.valid ? fmtNum(f.alt, ' m', 1) : '—'));
        grid.appendChild(kv('地面速度', f.valid ? fmtNum(f.speed_kph, ' km/h', 1) : '—'));
        grid.appendChild(kv('航向', f.valid ? fmtNum(f.course_deg, '°', 1) : '—'));
        grid.appendChild(kv('卫星数（参与定位）', (f.satellites === undefined ? '—' : f.satellites)));
        grid.appendChild(kv('水平精度 HDOP', fmtNum(f.hdop, '', 1)));
        grid.appendChild(kv('定位质量',
          GPS_QUALITY[f.fix_quality] || ('未知（' + f.fix_quality + '）')));
        grid.appendChild(kv('UTC 时间', utc.have
          ? (utc.year + '-' + pad2(utc.month) + '-' + pad2(utc.day) + ' ' +
             pad2(utc.hour) + ':' + pad2(utc.minute) + ':' + pad2(utc.second))
          : '—'));
        grid.appendChild(kv('数据时延', fmtAge(lk.age_ms),
          (lk.age_ms === null || lk.age_ms === undefined) ? 'err'
            : (lk.age_ms < 5000 ? 'ok' : 'warn')));

        /* 链路：串口开没开、收没收到字节。现场排障的两个分叉点 ——
         * rx_bytes=0 是线/波特率/设备树问题；有字节但不定位是天线/遮蔽问题。 */
        grid2.appendChild(kv('串口', (lk.path || '—') + '　' + (lk.open ? '已打开' : '未打开'),
          lk.open ? 'ok' : 'warn'));
        grid2.appendChild(kv('累计收到字节', (lk.rx_bytes === undefined ? '—' : lk.rx_bytes),
          (lk.rx_bytes ? 'ok' : 'warn')));
        grid2.appendChild(kv('解析成功语句', s.sentences_ok === undefined ? '—' : s.sentences_ok));
        grid2.appendChild(kv('有效定位更新', s.fix_updates === undefined ? '—' : s.fix_updates));
        grid2.appendChild(kv('校验错 / 结构错',
          (s.checksum_bad === undefined ? '—' : s.checksum_bad) + ' / ' +
          (s.malformed === undefined ? '—' : s.malformed)));
        grid2.appendChild(kv('超长丢弃 / 噪声字节',
          (s.too_long === undefined ? '—' : s.too_long) + ' / ' +
          (s.noise_bytes === undefined ? '—' : s.noise_bytes)));
        grid2.appendChild(kv('校时（成功 / 被拒）',
          (s.time_sync_ok === undefined ? '—' : s.time_sync_ok) + ' / ' +
          (s.time_sync_rejected === undefined ? '—' : s.time_sync_rejected)));
      }).catch(function () {
        grid.innerHTML = '';
        grid2.innerHTML = '';
        note.textContent = '';
        grid.appendChild(kv('状态', '无法连接服务', 'err'));
      });
    }

    btn.addEventListener('click', load);
    load();
    st.timer = setInterval(load, 5000);  /* 5 s 自动刷新；切页时由 go() 收掉 */
    return root;
  }

  /* 控制板（采集板）：数据来源 `GET /api/board` → ivsboxd 的 `board.status`。
   * 后端把镜像里"未填"的字段一律给 JSON null（valid 位图为 0）—— 采集板没接时
   * 页面上必须是一排"—"，不能被读成"输入电压 0V / 温度 0℃"。 */
  var DS_TEXT = { 1: '关闭', 2: '打开' };
  /* 继电器口径与门状态**相反**：RELAY 1=开 / 2=关（架构 §18.2，别套用 DS 的映射） */
  var RELAY_TEXT = { 1: '吸合（开）', 2: '断开（关）' };

  function pageBoard() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '控制板'));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '运行参数'));
    var note = el('p', 'hint', '读取中…');
    card.appendChild(note);
    var grid = el('div', 'grid');
    card.appendChild(grid);
    var bar = el('div', 'actions');
    var btn = el('button', 'ghost', '刷新');
    btn.type = 'button';
    bar.appendChild(btn);
    card.appendChild(bar);
    root.appendChild(card);

    var card2 = el('div', 'card');
    card2.appendChild(el('h3', null, '三路继电器'));
    /* 口径写在这里，不写进每个数值里：状态值 1=吸合 / 2=断开，与箱门 DS 相反。 */
    card2.appendChild(el('p', 'hint',
      '状态口径：吸合（开）=1，断开（关）=2（与箱门 DS 相反）。' +
      '控制走 0xD1，第 N 路对应采集板电源口 port N-1；下发后自动回读一次状态。'));
    var relayBox = el('div');
    card2.appendChild(relayBox);
    var relayMsg = el('p', 'hint', '');
    card2.appendChild(relayMsg);
    root.appendChild(card2);

    var card3 = el('div', 'card');
    card3.appendChild(el('h3', null, '告警阈值'));
    var hint3 = el('p', 'hint', '阈值随 0xE1 应答与 0xF1 下发共用同一批字段；温度下限可为负。');
    card3.appendChild(hint3);
    var grid3 = el('div', 'grid');
    card3.appendChild(grid3);
    root.appendChild(card3);

    var card4 = el('div', 'card');
    card4.appendChild(el('h3', null, '通信链路'));
    var grid4 = el('div', 'grid');
    card4.appendChild(grid4);
    root.appendChild(card4);

    function num(v, unit, digits) {
      if (v === null || v === undefined) return '—';
      return fmtNum(v, unit || '', digits === undefined ? 1 : digits);
    }

    function raw(v) {
      return (v === null || v === undefined) ? '—' : String(v);
    }

    /* 继电器控制按钮：**只发一次动作**，不在这里做"等确认再发"的补偿 —— 0xD1 是
     * 控制命令，重发可能让采集板重复动作（协议无事务号）。失败就把原因说清楚，
     * 人再决定要不要点第二次。 */
    var busy = false;

    function relayBtn(ch, on) {
      var b = el('button', 'ghost', on ? '吸合' : '断开');

      b.type = 'button';
      b.disabled = busy;
      b.addEventListener('click', function () {
        if (busy) return;
        if (!window.confirm('确认' + (on ? '吸合' : '断开') + '第 ' + ch + ' 路继电器？\n' +
            '（0xD1  port ' + (ch - 1) + '  status=' + (on ? 1 : 0) + '）')) return;
        busy = true;
        relayMsg.textContent = '下发中…';
        api('/api/board/relay', { method: 'POST', body: { ch: ch, on: on } })
          .then(function (res) {
            /* 注意：这里**不能**先把 busy 放回 false —— 否则下面 2.2 s / 5 s 的冷却
             * 形同虚设，人连点两路立刻撞上 0xD1 的"同命令在飞"。只有失败分支和
             * 5 s 定时器才解开。 */
            if (!res.ok) {
              busy = false;
              relayMsg.textContent = '下发失败：' + (res.msg || ('错误 ' + res.e)) +
                (res.e === -3 ? '（同命令在飞，稍后重试）' : '');
              return;
            }
            var rd = res.d || {};
            relayMsg.textContent = '已下发 ' + (rd.payload || '') + '，正在回读状态…';
            /* `0xD1` 同命令只允许一笔在飞（应答窗口 2 s），故按钮一起冷却 2.2 s：
             * 不放冷的话，人连着点两路必然收到"同命令在飞"，看起来像功能坏了。 */
            setTimeout(function () {
              load(); /* 后端发完已催一次 0xE1，这里回读确认动作真的生效 */
            }, 2200);
            /* **必须回读第二遍**：板端实测（2026-10-17）下发 ch=2 OFF 后 6 s 回读仍是
             * 旧值 —— 那条 0xE1 应答是在采集板处理 0xD1 之前就发出去的，镜像落后一拍。
             * 只回读一次会出现"按钮点了、表格没变"，看着像没生效。 */
            setTimeout(function () {
              busy = false;
              load();
              relayMsg.textContent = '已下发，状态已回读（采集板镜像可能滞后一拍）';
            }, 5000);
          })
          .catch(function () {
            busy = false;
            relayMsg.textContent = '无法连接服务';
          });
      });
      return b;
    }

    function load() {
      api('/api/board').then(function (r) {
        grid.innerHTML = '';
        relayBox.innerHTML = '';
        grid3.innerHTML = '';
        grid4.innerHTML = '';
        if (!r.ok) {
          note.textContent = '';
          var why = (r.msg === 'agent unavailable' || r.msg === 'board link unavailable')
            ? 'ivsboxd 未运行，或本机未装配采集板链路'
            : (r.msg || '读取失败');
          grid.appendChild(kv('状态', why, 'err'));
          return;
        }
        var d = r.d || {}, lk = d.link || {}, dd = d.data || {};
        var lim = d.limit || {}, rel = d.relay || [];

        /* 一句话结论：把"没接"和"接了但一帧都不对"分开 —— 这两种情况帧计数都是 0，
         * 现场只能靠"收到过字节没" + "数据年龄"区分，处置完全不同。 */
        if (!lk.open) {
          note.textContent = '采集板串口未打开：' + (lk.path || '（无路径）') +
            ' 打不开，请检查接线与设备节点。';
        } else if (!lk.rx_bytes) {
          note.textContent = '串口已打开，但一个字节都没收到 —— 检查采集板供电、接线（TX/RX 是否交叉）、波特率 115200。';
        } else if (!lk.frames) {
          note.textContent = '收到 ' + lk.rx_bytes + ' 字节，但没有一帧通过校验 —— 检查 CRC 覆盖范围、帧尾与协议版本（见下方错误计数）。';
        } else {
          note.textContent = '已收到 ' + lk.frames + ' 帧完整报文，最近一次数据 ' + fmtAge(lk.age_ms) + '。';
        }

        /* 运行参数：键名照搬 0xE1 应答（便于对照《单片机通信》表），标签给中文 */
        grid.appendChild(kv('输入电压 V', num(dd.V, ' V', 1)));
        grid.appendChild(kv('输入电流 A', num(dd.A, ' A', 2)));
        grid.appendChild(kv('箱内温度 T', num(dd.T, ' ℃', 1)));
        grid.appendChild(kv('箱内湿度 H', num(dd.H, ' %', 1)));
        grid.appendChild(kv('箱门 DS',
          dd.DS === null || dd.DS === undefined ? '—' : (DS_TEXT[dd.DS] || ('未知（' + dd.DS + '）')),
          dd.DS === 1 ? 'ok' : (dd.DS === 2 ? 'warn' : null)));
        grid.appendChild(kv('总功率 APOWER', dd.APOWER === null || dd.APOWER === undefined ? '—' : dd.APOWER));
        grid.appendChild(kv('总用电量 AKW', dd.AKW === null || dd.AKW === undefined ? '—' : dd.AKW));
        grid.appendChild(kv('姿态 P', raw(dd.P)));
        grid.appendChild(kv('防雷 SPD', raw(dd.SPD)));
        grid.appendChild(kv('设备供电 PA', raw(dd.PA)));
        grid.appendChild(kv('输入电压状态 PV', raw(dd.PV)));
        grid.appendChild(kv('累计开门次数', raw(d.door_events)));

        /* 三路继电器：**每一项单独一列**（状态/电压/电流/功率/电量各自可读），
         * 每行两个按钮直接下发 0xD1。状态与三路测量各有独立 valid 位，缺哪项就显示"—"。 */
        if (!rel.length) {
          relayBox.appendChild(el('p', 'hint', '尚未收到继电器数据（采集板未应答 0xE1）。'));
        } else {
          var t = el('table');
          var head = el('tr');
          ['通道', '状态', '电压', '电流', '功率', '电量', '控制'].forEach(function (h) {
            head.appendChild(el('th', null, h));
          });
          t.appendChild(head);
          for (var i = 0; i < rel.length; i++) {
            var x = rel[i] || {};
            var ch = i + 1;
            var tr = el('tr');
            tr.appendChild(el('td', null, '第 ' + ch + ' 路'));
            tr.appendChild(el('td', null,
              (x.state === null || x.state === undefined)
                ? '未知' : (RELAY_TEXT[x.state] || ('未知（' + x.state + '）'))));
            tr.appendChild(el('td', null, num(x.v, ' V', 1)));
            tr.appendChild(el('td', null, num(x.a, ' A', 2)));
            tr.appendChild(el('td', null, num(x.power, ' W', 1)));
            tr.appendChild(el('td', null, num(x.elec, '', 2)));
            var td = el('td');
            td.appendChild(relayBtn(ch, true));
            td.appendChild(relayBtn(ch, false));
            tr.appendChild(td);
            t.appendChild(tr);
          }
          relayBox.appendChild(t);
        }

        grid3.appendChild(kv('电压上限 hv', raw(lim.hv)));
        grid3.appendChild(kv('电压下限 lv', raw(lim.lv)));
        grid3.appendChild(kv('过流 ov', raw(lim.ov)));
        grid3.appendChild(kv('温度上限 tu', raw(lim.tu)));
        grid3.appendChild(kv('温度下限 tl', raw(lim.tl)));
        grid3.appendChild(kv('湿度上限 hu', raw(lim.hu)));
        grid3.appendChild(kv('湿度下限 hl', raw(lim.hl)));
        grid3.appendChild(kv('姿态 sl', raw(lim.sl)));
        grid3.appendChild(kv('漏电流 ld', raw(lim.ld)));

        grid4.appendChild(kv('串口', (lk.path || '—') + '　' + (lk.open ? '已打开' : '未打开'),
          lk.open ? 'ok' : 'warn'));
        grid4.appendChild(kv('链路状态', lk.up ? '已建立（收到过合法帧）' : '未建立',
          lk.up ? 'ok' : 'warn'));
        grid4.appendChild(kv('数据年龄', fmtAge(lk.age_ms),
          (lk.age_ms === null || lk.age_ms === undefined) ? 'err'
            : (lk.age_ms < 35000 ? 'ok' : 'warn')));
        grid4.appendChild(kv('完整帧 / 复位',
          raw(lk.frames) + ' / ' + raw(lk.resets)));
        grid4.appendChild(kv('CRC 错 / 帧尾错',
          raw(lk.crc_err) + ' / ' + raw(lk.tail_err),
          (lk.crc_err || lk.tail_err) ? 'warn' : null));
        grid4.appendChild(kv('超长丢弃 / 噪声字节',
          raw(lk.too_long) + ' / ' + raw(lk.noise)));
        grid4.appendChild(kv('收 / 发字节',
          raw(lk.rx_bytes) + ' / ' + raw(lk.tx_bytes)));
        grid4.appendChild(kv('串口错误 / 发送丢弃',
          raw(lk.serial_err) + ' / ' + raw(lk.tx_drop),
          lk.serial_err ? 'warn' : null));
        grid4.appendChild(kv('下行轮询 / 上报 / 心跳',
          raw(lk.polls) + ' / ' + raw(lk.reports) + ' / ' + raw(lk.heartbeats)));
        grid4.appendChild(kv('查询应答 / 待发队列',
          raw(lk.query_rsp) + ' / ' + raw(lk.queue) +
          (lk.queue_full ? '（队满丢弃 ' + lk.queue_full + '）' : '')));
      }).catch(function () {
        grid.innerHTML = '';
        relayBox.innerHTML = '';
        grid3.innerHTML = '';
        grid4.innerHTML = '';
        note.textContent = '';
        grid.appendChild(kv('状态', '无法连接服务', 'err'));
      });
    }

    btn.addEventListener('click', load);
    load();
    st.timer = setInterval(load, 5000);  /* 5 s 自动刷新；切页时由 go() 收掉 */
    return root;
  }

  /* ==================== 摄像机通道建档（D-06，2026-10-21） ====================
   * camera.ch.N.* 在配置模块里是「按需变长数组」：出厂一条都没有，
   * 只有当 config.apply 里出现 camera.ch.N.<某键> 时，set_post_update() 才抬高
   * camera_count 并把该槽 8 个键整套补默认值（ip/mac/pass/record/stream/url/
   * user/vendor）。
   * 而 configCard() 只遍历 config.class 返回的既有键 —— 既有键为空就一个输入框
   * 都没有，于是现场从网页永远建不出第一路摄像机，只能等平台下发或手改 JSON。
   * 这里补的就是这个入口：只发一个 ip 键触发建槽，其余 7 项交给后端补齐。
   */
  var CAM_SLOT_MAX = 8;   /* 与录像 8 路对齐（CNS 只测前 6 路 ⇒ 7/8 路只录不测） */

  function cameraSlotCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '新增摄像机通道'));
    card.appendChild(el('p', 'hint',
      '设备出厂未配置任何通道。选一个序号、填上摄像机 IP，点「建立」即建好这一路；' +
      '账号／口令／端口／录像等其余字段自动取默认值，建完后可在上方表格里继续修改。'));

    var bar = el('div', 'actions');
    var sel = document.createElement('select');
    sel.title = '通道序号';
    var ip = document.createElement('input');
    ip.type = 'text';
    ip.placeholder = '摄像机 IP，如 192.168.1.64';
    ip.size = 24;
    var btn = el('button', null, '建立该路通道');
    btn.type = 'button';
    bar.appendChild(sel);
    bar.appendChild(ip);
    bar.appendChild(btn);
    card.appendChild(bar);
    card.appendChild(el('p', 'hint', '通道序号超出实际路数不影响使用：只录不测（检测固定 6 路）。'));

    var known = {};        /* 已存在的槽位序号 -> ip 现值（null = 槽在但 ip 为空） */
    var onSaved = null;    /* 建槽成功后刷新上方的通道表格 */

    function fillIp() {
      var cur = known[Number(sel.value)];
      ip.value = (cur === undefined || cur === null) ? '' : String(cur);
    }

    /* 按已知槽位重建下拉：既要说清哪几路已配置，又要保住用户当前选中项 */
    function renderSlots() {
      var keep = sel.value;
      sel.innerHTML = '';
      for (var n = 0; n < CAM_SLOT_MAX; n++) {
        var o = document.createElement('option');
        o.value = String(n);
        o.textContent = '通道 ' + (n + 1) +
          (known[n] === undefined ? '（未配置）' : '（已配置：' + (known[n] || '无 IP') + '）');
        sel.appendChild(o);
      }
      if (keep !== '') sel.value = keep;
      fillIp();
    }

    sel.addEventListener('change', fillIp);

    btn.addEventListener('click', function () {
      var n = Number(sel.value);
      var addr = (ip.value || '').replace(/^\s+|\s+$/g, '');
      if (!addr) { toast('先填摄像机 IP', 'err'); return; }
      var items = {};
      items['camera.ch.' + n + '.ip'] = addr;
      btn.disabled = true;
      api('/api/config/apply', { method: 'POST', body: { ver: st.ver, items: items } })
        .then(function (r) {
          btn.disabled = false;
          if (r.ok) {
            st.ver = r.d.version;
            toast('已建立通道 ' + (n + 1) + '（版本 ' + r.d.version + '）', 'ok');
            if (onSaved) onSaved();
          } else if (r.e === -3) {
            toast('配置已被别处改动，请刷新后重试', 'err');
          } else {
            toast('建立失败：' + (r.msg || r.e), 'err');
          }
        })
        .catch(function () { btn.disabled = false; toast('请求失败', 'err'); });
    });

    renderSlots();
    return {
      node: card,
      /* configCard 每次载入完都会调这里：同步已知槽位 + 拿到刷新手柄 */
      sync: function (items, reload) {
        onSaved = reload;
        known = {};
        Object.keys(items || {}).forEach(function (k) {
          var m = /^camera\.ch\.(\d+)\./.exec(k);
          if (!m) return;
          var idx = Number(m[1]);
          if (known[idx] === undefined) known[idx] = null;   /* 槽在，ip 未必在 */
          if (/\.ip$/.test(k)) known[idx] = items[k];
        });
        renderSlots();
      }
    };
  }

  /* ==================== 摄像机检测方式 / 单路重测 / 查摄像机（2026-10-22） ====================
   * 补齐参考 web 的三件事（蓝本：index.shtml 的「检测方式」下拉 ＋ 系统操作里的
   * 「查摄像机1」）：
   *
   *   ① **检测方式**：参考 web 是三选一（PING方式 / 端口扫描 / RTSP拉流测试），
   *      保存即生效。这三档在 `iv_camdet` 里早就实现了，但配置键
   *      `camera.detect.mode` 原先只在 ivsboxd **启动时**读一次 ⇒ 网页上改完必须
   *      重启主控，等于摆设。2026-10-22 补了装配层热生效
   *      （`on_cfg_committed()` 的 CAMERA 分支 → `iv_agent_reload_camdet()`），
   *      这里才给它一个正经下拉框，而不是让人去配置表格里填 1/2/3。
   *   ② **单路「测」**：camdet 是轮询式（PING 20 s 一轮、扫描/RTSP 30 s 一轮），
   *      现场改完 IP 想马上知道这一路通不通只能干等一轮。按钮打
   *      `camera.test{ch}`，后端立刻重开该路探测，并给出 `expect_ms` 让本页定时回读。
   *   ③ **查摄像机（SNMP）**：参考 web 的「查摄像机1」就是读第 1 路摄像机的 SNMP
   *      槽位。后端 `snmp.run`/`snmp.result` 早就通了，但入口原先只在「交换机」页
   *      —— 摄像机的东西放摄像机页才找得到。
   */
  var CD_MODE_OPT = [[1, 'PING方式'], [2, '端口扫描'], [3, 'RTSP拉流测试']];

  /* 检测方式卡：下拉 ＋「保存并立即生效」。写成独立卡片而不是塞进 configCard，
   * 是因为 configCard 只会渲染成"键=输入框"，让人手填 1/2/3 在现场等于没做。 */
  function cameraModeCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '检测方式'));
    card.appendChild(el('p', 'hint',
      '三种方式互斥、保存即生效（不必重启主控）。' +
      'PING＝每路 4 个回包／20 秒，能分辨「延时」「丢包」；' +
      '端口扫描＝逐台试 7 个常见端口（80/554/8000/37777/5000/8080/8899），只分「正常／故障」；' +
      'RTSP拉流测试＝向 rtsp://<ip>:554/ 发一个 OPTIONS，收到任何响应即「正常」（不拉流、不解析 SDP）。'));

    var bar = el('div', 'actions');
    var sel = document.createElement('select');
    var i;
    for (i = 0; i < CD_MODE_OPT.length; i++) {
      var op = document.createElement('option');
      op.value = String(CD_MODE_OPT[i][0]);
      op.textContent = CD_MODE_OPT[i][1];
      sel.appendChild(op);
    }
    var btn = el('button', null, '保存并立即生效');
    btn.type = 'button';
    bar.appendChild(sel);
    bar.appendChild(btn);
    card.appendChild(bar);

    var onSaved = null;

    btn.addEventListener('click', function () {
      var items = {};
      items['camera.detect.mode'] = Number(sel.value);
      btn.disabled = true;
      api('/api/config/apply', { method: 'POST', body: { ver: st.ver, items: items } })
        .then(function (r) {
          btn.disabled = false;
          if (r.ok) {
            st.ver = r.d.version;
            toast('检测方式已切到「' + sel.options[sel.selectedIndex].text + '」', 'ok');
            if (onSaved) onSaved();
          } else if (r.e === -3) {
            toast('配置已被别处改动，请刷新后重试', 'err');
          } else {
            toast('保存失败：' + (r.msg || r.e), 'err');
          }
        })
        .catch(function () { btn.disabled = false; toast('请求失败', 'err'); });
    });

    return {
      node: card,
      /* 回填当前生效的方式（来自 /api/overview 的 cameras.mode = camdet 的真实状态，
       * 不是配置里的值 —— 两者在"改了但没生效"时会不一致，而这里要的是后者）。 */
      showMode: function (m) { if (m) sel.value = String(m); },
      onSaved: function (fn) { onSaved = fn; }
    };
  }

  /* 六路 CNS（只读实时）＋ 每路一个「测」按钮。
   * 「测」是"已排上"而不是"已出结论"：camdet 是非阻塞状态机，ctl 同步等结果会
   * 把 Reactor 卡住。所以后端返回 `expect_ms`（PING 20 s／扫描 8 s／RTSP 1.5 s），
   * 本卡按它定时回读一次。 */
  function cameraStatusCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '摄像机状态（实时）'));
    var hint = el('p', 'hint', '读取中…');
    card.appendChild(hint);
    var grid = el('div', 'grid');
    card.appendChild(grid);

    var rows = [];
    var onMode = null;

    function testOne(idx, btn) {
      btn.disabled = true;
      api('/api/camera/test', { method: 'POST', body: { ch: idx + 1 } })
        .then(function (r) {
          btn.disabled = false;
          if (!r.ok) {
            /* -10 = IV_ENOENT：这一路还没配 IP（**不是** -2，那是 IV_EINVAL＝参数非法）。
             * 现场点「测」最常撞的就是它，报"probe rejected"等于让人去翻日志。 */
            if (r.e === -10) toast('摄像机' + (idx + 1) + ' 还没配 IP，先在下方建立通道', 'err');
            else toast('测失败：' + (r.msg || r.e), 'err');
            return;
          }
          var wait = (r.d && r.d.expect_ms) ? r.d.expect_ms : 3000;
          var mn = (r.d && r.d.mode_name) ? r.d.mode_name : '';
          toast('已在测摄像机' + (idx + 1) + '（' + mn + '），约 ' +
            Math.round(wait / 1000) + ' 秒后自动刷新', 'ok');
          setTimeout(load, wait);
        })
        .catch(function () { btn.disabled = false; toast('请求失败', 'err'); });
    }

    function load() {
      api('/api/overview').then(function (r) {
        var cm = (r && r.ok && r.d) ? r.d.cameras : null;

        if (!cm || !cm.cns || !cm.cns.length) {
          hint.textContent = '未获取到摄像机状态（检测模块未装配或尚未开始检测）。';
          return;
        }
        hint.textContent = '判定口径：0 无IP／1 正常／2 故障／4 延时／5 丢包。' +
          '「测」＝立刻用当前检测方式重测这一路，不必等下一轮。';
        for (var k = 0; k < cm.cns.length; k++) {
          var v = CNS_NAME[cm.cns[k]] === undefined ? cm.cns[k] : CNS_NAME[cm.cns[k]];
          var cls = cm.cns[k] === 1 ? 'ok' : (cm.cns[k] === 2 ? 'err' : 'warn');
          if (!rows[k]) {
            var box = el('div', 'kv');
            box.appendChild(el('div', 'k', '摄像机' + (k + 1)));
            var vn = el('div', 'v', v);
            box.appendChild(vn);
            var b = el('button', 'ghost', '测');
            b.type = 'button';
            (function (idx, bb) {
              bb.addEventListener('click', function () { testOne(idx, bb); });
            })(k, b);
            box.appendChild(b);
            grid.appendChild(box);
            rows[k] = { btn: b, value: vn };
          }
          rows[k].value.textContent = v;
          rows[k].value.className = 'v ' + cls;
        }
        if (onMode) onMode(cm.mode);
      });
    }

    load();
    return {
      node: card,
      reload: load,
      onMode: function (fn) { onMode = fn; }
    };
  }

  /* SNMP 采集结果卡（摄像机页与交换机页共用一份，避免两处两套话术）。
   * `which`：cam / sw（口径见 iv_ctl 的 snmp_which()）。 */
  function snmpCard(which, title, hintText) {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, title));
    if (hintText) card.appendChild(el('p', 'hint', hintText));

    var bar = el('div', 'actions');
    var btn = el('button', null, '发起采集');
    btn.type = 'button';
    bar.appendChild(btn);
    card.appendChild(bar);

    var box = el('div');
    card.appendChild(box);

    function show() {
      box.innerHTML = '读取中…';
      api('/api/snmp?which=' + which).then(function (r) {
        box.innerHTML = '';
        if (!r.ok) { box.appendChild(el('p', 'msg', r.msg || '读取失败')); return; }
        /* `d`/`slots` 逐层兜底（R-07）：服务端回 `ok:true` 但缺字段时（旧版
         * 守护进程、或 /api/snmp 还没接上），原先 `r.d.slots.length` 直接抛
         * TypeError，异常把整个页面的后续渲染一起带走 —— 页面看着"点了一下就
         * 白了"，控制台只有一句 cannot read property。 */
        var d = r.d || {};
        var slots = d.slots || [];
        box.appendChild(el('p', 'hint', '状态：' + (SNMP_STATE[d.state] || d.state)));
        if (!slots.length) { box.appendChild(el('p', 'hint', '尚无采集结果')); return; }
        var t = el('table');
        var head = el('tr');
        ['槽位', 'OID', '行数', '取值'].forEach(function (h) { head.appendChild(el('th', null, h)); });
        t.appendChild(head);
        slots.forEach(function (s) {
          var tr = el('tr');
          tr.appendChild(el('td', null, s.name));
          tr.appendChild(el('td', null, s.oid));
          tr.appendChild(el('td', null, s.n_rows));
          var vals = (s.rows || []).map(function (row) {
            if (row.str) return row.str;
            return String(row.num);
          }).join(' / ');
          tr.appendChild(el('td', null, vals || '—'));
          t.appendChild(tr);
        });
        box.appendChild(t);
      });
    }

    btn.addEventListener('click', function () {
      api('/api/snmp/run', { method: 'POST', body: { which: which } }).then(function (r) {
        toast(r.ok ? '已发起采集' : ('失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
        if (r.ok) setTimeout(show, 2500);
      });
    });

    show();
    return { node: card, reload: show };
  }

  /* ==================== 摄像机通道参数（2026-10-27 用户口径） ====================
   * 五条口径：
   *   ① 用户名/密码**明文**（首版不加密；F8 密钥分离仍未做，属已知欠账）；
   *   ② 码流类型用下拉，不再让人手填 `main`/`sub` 这种协议内部词；
   *   ③ 品牌下拉：海康／大华／宇视／华为／中维世纪，未选＝海康；
   *   ④ 拉流地址按「品牌＋用户名＋密码＋IP」**自动生成**（仍可手改）；
   *   ⑤ **不配 MAC**：MAC 是发现阶段的产物，不是现场该填的配置。
   *
   * 为什么不再用 configCard()：那张卡是"后端给什么键就画什么框"，
   * 做不出下拉，也做不出"改 IP 就重算地址"这种联动。它的价值在于后端加键前端
   * 零改动，而摄像机这 8 个字段是**已经定型的业务字段**，值得专门的表单。
   */
  var CAM_BRAND = [
    /* 路径取自各厂公开的 RTSP 取流格式（2026-10-27 核对）。**固件版本不同会有差异**，
     * 所以生成出来的地址永远是"可改的草稿"，不是唯一答案。 */
    { v: 'hik',      label: '海康',     port: 554, main: '/Streaming/Channels/101',
      sub: '/Streaming/Channels/102' },
    { v: 'dahua',    label: '大华',     port: 554,
      main: '/cam/realmonitor?channel=1&subtype=0',
      sub: '/cam/realmonitor?channel=1&subtype=1' },
    { v: 'uniview',  label: '宇视',     port: 554, main: '/media/video1',
      sub: '/media/video2' },
    { v: 'huawei',   label: '华为',     port: 554, main: '/LiveMedia/ch1/Media1',
      sub: '/LiveMedia/ch1/Media2' },
    /* 中维世纪两套写法都常见：`554/av0_0`（多数 IPC／NVR）与 `8554/live0.264`
     * （部分机型）。这里取公开资料里成对给出的后者；前者写在页面提示里，
     * 现场拉不通就直接改这一格地址。 */
    { v: 'jovision', label: '中维世纪', port: 8554, main: '/live0.264',
      sub: '/live1.264' }
  ];
  /* 「（默认：主码流）」这一项是显式的空值：`camera.ch.N.stream` 的出厂值就是空串，
   * 拼地址时按主码流处理 —— 与其在界面上显示"未设置"，不如直接说清它等于主码流。 */
  var CAM_STREAM_OPT = [['', '（默认：主码流）'], ['main', '主码流（main）'],
    ['sub', '子码流（sub）']];
  var CAM_BRAND_OPT = [['', '（默认：海康）']].concat(CAM_BRAND.map(function (b) {
    return [b.v, b.label];
  }));

  /* 设备发现报的品牌 → 配置键 `camera.ch.N.vendor` 的取值（2026-10-29）。
   *
   * 这是两套**互不相同**的命名，别指望字符串直接对上：
   *   `iv_device.c:iv_dev_brand_name()` ⇒ hikvision / dahua / unv / onvif / unknown
   *   `camera.ch.N.vendor` 的取值域     ⇒ hik / dahua / uniview / huawei / jovision（空＝海康）
   * 少了这层映射，`select.value = 'hikvision'` 在下拉里找不到对应项，浏览器会把
   * select 留空 ⇒ 界面回落"（默认：海康）"—— 看起来就是"品牌识别出来了却没匹配"。
   * `onvif`（泛 ONVIF）与 `unknown` **故意不映射**：前端没有这一项，硬填只会按
   * 某个厂商的路径生成一条大概率拉不通的地址，交给用户手选更诚实。 */
  var DEV_BRAND_VENDOR = { hikvision: 'hik', dahua: 'dahua', unv: 'uniview' };

  /* 按 IP 去「设备档案」（发现结果）里找那一台。页面每路通道都要查一次，
   * 缓存 5 s 免得 8 路打 8 次 `/api/devices`。返回 Promise<item|null>。 */
  var devListCache = null, devListAt = 0, devListP = null;
  function devicesByIp(ip) {
    var p;
    ip = String((ip === undefined || ip === null) ? '' : ip).replace(/^\s+|\s+$/g, '');
    if (!ip) return Promise.resolve(null);
    if (devListCache && (Date.now() - devListAt) < 5000) {
      p = Promise.resolve(devListCache);
    } else if (devListP) {
      /* **飞行中的请求要复用**：8 路通道几乎同时建卡片，只靠"5 s 结果缓存"
       * 挡不住 —— 第一个请求还没回来时，另外 7 路都会各发一次（首版就这么
       * 被用例 v06a 抓到：3 路打了 3 次）。 */
      p = devListP;
    } else {
      devListP = api('/api/devices').then(function (r) {
        devListCache = (r && r.ok && r.d && r.d.items) ? r.d.items : [];
        devListAt = Date.now();
        devListP = null;
        return devListCache;
      }).catch(function () {
        /* 拉不到就当"没发现过"：自动匹配是锦上添花，绝不能因为它让页面报错 */
        devListCache = []; devListAt = Date.now(); devListP = null; return devListCache;
      });
      p = devListP;
    }
    return p.then(function (list) {
      var i, it, want = ip;
      for (i = 0; i < (list || []).length; i++) {
        it = list[i];
        if (it && String(it.ip || '').replace(/^\s+|\s+$/g, '') === want) return it;
      }
      return null;
    });
  }

  /* 拼 RTSP 地址。IP 为空 ⇒ 返回空串（不生成半截地址，免得被当成"已配好"）。
   * 没品牌／品牌不认识 ⇒ 海康（用户口径：没有品牌默认用海康）。
   * 用户名密码**原样拼**，不做百分号编码：多数 IPC 不认 `%XX`，反而认证失败；
   * 含 `@`/`:` 的密码会拼出语法歧义的地址 —— 登记为已知限制，现场口令别带这两个字符。 */
  function camUrlOf(brand, stream, user, pass, ip) {
    var b = null, i, auth = '';

    ip = String((ip === undefined || ip === null) ? '' : ip).replace(/^\s+|\s+$/g, '');
    if (!ip) return '';
    for (i = 0; i < CAM_BRAND.length; i++) {
      if (CAM_BRAND[i].v === brand) { b = CAM_BRAND[i]; break; }
    }
    if (!b) b = CAM_BRAND[0];
    user = String((user === undefined || user === null) ? '' : user).replace(/^\s+|\s+$/g, '');
    pass = String((pass === undefined || pass === null) ? '' : pass);
    if (user) auth = user + (pass ? ':' + pass : '') + '@';
    return 'rtsp://' + auth + ip + ':' + b.port + (stream === 'sub' ? b.sub : b.main);
  }

  function cameraParamsCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '摄像机通道参数'));
    card.appendChild(el('p', 'hint',
      '每路填 IP、用户名、密码，选品牌与码流类型；拉流地址按「品牌＋账号＋IP」自动生成，' +
      '固件不一致时可以直接改这一格（录像侧读的就是它）。' +
      '密码按明文保存（首版不加密）。中维世纪部分机型是 `8554/live0.264`、' +
      '部分机型要 `554/av0_0`，拉不通就改地址。MAC 不在这里配 —— 它由摄像机发现过程自动获得。'));

    var body = el('div');
    card.appendChild(body);

    var bar = el('div', 'actions');
    var save = el('button', null, '保存修改');
    save.type = 'button';
    var reload = el('button', 'ghost', '重新载入');
    reload.type = 'button';
    bar.appendChild(save); bar.appendChild(reload);
    card.appendChild(bar);

    /* 一路一个区块。`camera.ch.N.*` 是"按需变长数组"：哪一路有键就画哪一路；
     * 建槽入口在下面的「新增摄像机通道」卡片（只发 ip 一个键，其余由后端补齐）。 */
    function channelSection(n, items) {
      var pre = 'camera.ch.' + n + '.';
      var tag = '第 ' + (n + 1) + ' 路';
      function get(f) {
        var v = items[pre + f];
        return (v === undefined || v === null) ? '' : v;
      }

      var sec = el('div', 'chan-sec');
      sec.appendChild(el('h3', null, tag + '（camera.ch.' + n + '）'));

      var grid = el('div', 'grid');
      var labIp = fieldFor(pre + 'ip', get('ip'), tag + ' IP');
      var labUser = fieldFor(pre + 'user', get('user'), tag + ' 用户名（明文）');
      var labPass = fieldFor(pre + 'pass', get('pass'), tag + ' 密码（明文）');
      var labBrand = fieldSelect(pre + 'vendor', get('vendor'), CAM_BRAND_OPT, tag + ' 品牌');
      var labStream = fieldSelect(pre + 'stream', get('stream'), CAM_STREAM_OPT, tag + ' 码流类型');
      /* 「是否录像」也用下拉（2026-10-27 用户要求）：勾选框 + 右侧"已启用/已停用"
       * 在窄列里容易被读成"这一路的标记"，而"启用/停用"两选项没有歧义。
       * 值仍是布尔（`kind='boolean'`），不是字符串 —— 见 `collectChanges()`。 */
      var labRec = fieldSelect(pre + 'record', get('record') === true,
                               [['true', '启用'], ['false', '停用']],
                               tag + ' 是否录像', 'boolean');
      grid.appendChild(labIp);
      grid.appendChild(labUser);
      grid.appendChild(labPass);
      grid.appendChild(labBrand);
      grid.appendChild(labStream);
      grid.appendChild(labRec);
      sec.appendChild(grid);

      /* 地址单独占一行（它最长），且**跟着上面几个字段联动**。 */
      var labUrl = fieldFor(pre + 'url', get('url'), tag + ' 拉流地址（自动生成，可改）');
      sec.appendChild(labUrl);

      var ipInp = labIp.querySelector('input');
      var userInp = labUser.querySelector('input');
      var passInp = labPass.querySelector('input');
      var brandSel = labBrand.querySelector('select');
      var streamSel = labStream.querySelector('select');
      var urlInp = labUrl.querySelector('input');

      /* 手动改过地址就不再自动覆盖 —— 否则"现场手调好的地址"会被下一次改密码的
       * 联动冲掉。把这一格清空＝交回自动生成。 */
      var manual = false;

      function regen() {
        if (manual) return;
        var v = camUrlOf(brandSel.value, streamSel.value, userInp.value,
                         passInp.value, ipInp.value);
        urlInp.value = v;
        urlInp.className = (v === JSON.parse(urlInp.dataset.orig)) ? '' : 'dirty';
      }

      ipInp.addEventListener('input', regen);
      userInp.addEventListener('input', regen);
      passInp.addEventListener('input', regen);
      brandSel.addEventListener('change', regen);
      streamSel.addEventListener('change', regen);
      urlInp.addEventListener('input', function () { manual = (urlInp.value !== ''); });

      /* 老配置里地址为空、IP 已填 ⇒ 进页面就把草稿摆出来（此时还没落盘，
       * 用户点「保存修改」才会写进配置；`dirty` 高亮就是"待保存"）。 */
      if (urlInp.value === '' && ipInp.value !== '') regen();

      /* ------------------------------------------------------------------
       * 品牌自动匹配（2026-10-29，用户现场提问："搜索到摄像机、也解析出品牌了，
       * 配完 IP 为什么品牌不自动匹配？"）
       *
       * 只在两个时机查：① 进页面（配置里 vendor 为空但 IP 已填）；② IP 这一格
       * 提交（失焦 / 回车）。**不用 input 事件** —— 敲一个字符打一次
       * `/api/devices` 没有意义。
       *
       * 三条边界（少一条现场就会骂人）：
       *   · 用户手选过品牌（userPicked）就不再覆盖 —— 否则"我明明选了华为"会被改回去；
       *   · 发现结果只有泛 ONVIF / 未知 ⇒ **只提示、不改下拉**：填了也只能按某家
       *     的路径拼地址，一半概率拉不通，不如让用户自己选；
       *   · 手改过拉流地址（manual）时只换品牌、不重算地址，不覆盖现场手调的串。
       * ------------------------------------------------------------------ */
      var autoVendor = '';   /* 上一次由程序填进去的值 */
      var userPicked = false;
      var brandHint = el('p', 'hint', '');
      brandHint.style.display = 'none';
      sec.appendChild(brandHint);

      function brandLabel(v) {
        var k;
        for (k = 0; k < CAM_BRAND.length; k++)
          if (CAM_BRAND[k].v === v) return CAM_BRAND[k].label;
        return v;
      }

      function brandSay(msg) {
        if (!msg) { brandHint.textContent = ''; brandHint.style.display = 'none'; return; }
        brandHint.textContent = msg;
        brandHint.style.display = '';
      }

      function tryMatch() {
        var ip = String(ipInp.value || '').replace(/^\s+|\s+$/g, '');
        if (!ip || userPicked) return;
        devicesByIp(ip).then(function (dev) {
          if (!dev) { brandSay(''); return; }
          var v = DEV_BRAND_VENDOR[String(dev.brand || '')];
          if (!v) {
            brandSay('设备档案里这台只识别到「' + (dev.brand || '未知') +
                     '」（型号 ' + (dev.model || '—') + '），判断不出厂商，' +
                     '请手动选品牌 —— 现在按海康拼地址可能是错的。');
            return;
          }
          brandSay('已按设备档案自动选择品牌：' + brandLabel(v) + '（' + ip +
                   '），点「保存修改」后写进配置。');
          if (brandSel.value === v) return;
          brandSel.value = v;
          autoVendor = v;
          /* `fieldSelect()` 只在 change 事件里刷 dirty：程序赋值不派发事件，
           * 所以这里手动补一次，让"待保存"的高亮与手选时一致。 */
          brandSel.className = (v === JSON.parse(brandSel.dataset.orig)) ? '' : 'dirty';
          regen();
        }).catch(function () { brandSay(''); });
      }

      brandSel.addEventListener('change', function () {
        if (brandSel.value !== autoVendor) { userPicked = true; brandSay(''); }
      });
      ipInp.addEventListener('change', tryMatch);
      if (String(get('vendor') || '') === '' && ipInp.value !== '') tryMatch();

      return sec;

    }

    function load() {
      body.innerHTML = '';
      api('/api/config?cls=camera').then(function (r) {
        if (!r || !r.ok) {
          body.appendChild(el('p', 'msg', (r && r.msg) || '配置不可用'));
          return;
        }
        st.ver = r.d.version;
        var items = r.d.items || {};
        var idx = [], seen = {};
        Object.keys(items).forEach(function (k) {
          var m = /^camera\.ch\.(\d+)\./.exec(k);
          if (m === null) return;
          var n = Number(m[1]);
          if (!seen[n]) { seen[n] = 1; idx.push(n); }
        });
        idx.sort(function (a, b) { return a - b; });
        if (!idx.length) {
          body.appendChild(el('p', 'hint',
            '尚未配置任何通道 —— 用下方的「新增摄像机通道」建第一路。'));
          return;
        }
        idx.forEach(function (n) { body.appendChild(channelSection(n, items)); });
      }).catch(function () {
        body.innerHTML = '';
        body.appendChild(el('p', 'msg', '请求失败'));
      });
    }

    save.addEventListener('click', function () { applyChanges(body, load); });
    reload.addEventListener('click', load);
    load();
    return card;
  }

  function pageCamera() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '摄像机'));

    /* 六路 CNS 与检测方式原先在总览页，现归本页：状态和"这一路配的是哪个 IP"
     * 放在一起才有用 —— 看到"3 号故障"紧接着就能核对它的 IP/账号。
     * CNS 口径：0 无IP / 1 正常 / 2 故障 / 4 延时 / 5 丢包（无 3）。 */
    var status = cameraStatusCard();
    root.appendChild(status.node);

    var modeCard = cameraModeCard();
    status.onMode(function (m) { modeCard.showMode(m); });
    modeCard.onSaved(function () { status.reload(); });
    root.appendChild(modeCard.node);

    root.appendChild(snmpCard('cam', '查摄像机（SNMP）',
      '读第 1 路摄像机（camera.ch.0.ip）的 SNMP 槽位 —— 对应参考 web 系统操作里的' +
      '「查摄像机1」。采集是阻塞式 UDP 事务，落在慢任务池里跑，点完约 2~3 秒回读结果。').node);

    /* 通道参数走专用卡片（2026-10-27）：品牌/码流要是下拉、拉流地址要能自动生成、
     * MAC 不给入口 —— 通用表单只会渲染成"键＝输入框"，这三件事都做不到。 */
    root.appendChild(cameraParamsCard());

    var slot = cameraSlotCard();
    /* 通用卡片从此刻起只管 `camera.ch.*` **之外**的键（检测参数、录像节拍、
     * SNMP 连接与 OID 槽位）。通道那 8 个字段已经在新卡片里画过，
     * 再画一遍就是同一个键两个编辑器 ⇒ 后提交的那个必然带过期 `st.ver`、撞 e=-3。 */
    root.appendChild(configCard('检测、录像与 SNMP 参数（camera.*）', ['camera'],
      'OID 槽位按机型填值；录像节拍与检测参数在此。通道（IP／账号／品牌／码流／拉流地址）见上方卡片。',
      function (items, reload) { slot.sync(items, reload); },
      ['camera.ch.*']));
    root.appendChild(slot.node);
    return root;
  }

  function pageDevices() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '设备档案'));
    var card = el('div', 'card');
    var bar = el('div', 'actions');
    var btn = el('button', null, '重新搜索摄像机');
    btn.type = 'button';
    bar.appendChild(btn);
    card.appendChild(bar);
    var box = el('div');
    card.appendChild(box);
    root.appendChild(card);

    function load() {
      box.innerHTML = '';
      api('/api/devices').then(function (r) {
        if (!r.ok) { box.appendChild(el('p', 'msg', r.msg || '读取失败')); return; }
        if (!r.d.count) { box.appendChild(el('p', 'hint', '尚未发现设备（可点上方按钮重新搜索）')); return; }
        var t = el('table');
        var head = el('tr');
        ['品牌', 'IP', 'MAC', '型号', '序列号', '端口', '发现方式'].forEach(function (h) {
          head.appendChild(el('th', null, h));
        });
        t.appendChild(head);
        r.d.items.forEach(function (d) {
          var tr = el('tr');
          [d.brand, d.ip, d.mac, d.model, d.serial, d.port, d.found_by].forEach(function (v) {
            tr.appendChild(el('td', null, v === undefined || v === '' ? '—' : v));
          });
          t.appendChild(tr);
        });
        box.appendChild(t);
      });
    }
    btn.addEventListener('click', function () {
      api('/api/camera/search', { method: 'POST', body: {} }).then(function (r) {
        toast(r.ok ? '已触发搜索，稍后刷新' : ('搜索失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
      });
    });
    load();
    return root;
  }

  function pageSwitch() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '交换机'));
    root.appendChild(configCard('交换机参数与 OID（switch.*）', ['switch'],
      'OID 槽位键名即语义（厂家中立），值按实际机型填。'));

    /* 「采集摄像机」原先也在这张卡上，2026-10-22 挪到摄像机页（`snmpCard('cam')`）：
     * 摄像机的东西在摄像机页才找得到，而且那边正好挨着"这一路通不通"。 */
    root.appendChild(snmpCard('sw', 'SNMP 采集结果',
      '按 switch.* 里的 OID 槽位读交换机（标准 IF-MIB ＋ 厂商私有节点）。').node);
    return root;
  }

  /* 抓拍卡（USB 摄像头，2026-10-28）：`POST /api/snapshot` ＋ `/media/snap/`。
   *
   * 三条界面上的约定（都不显眼，但错一条现场就会误判功能坏了）：
   *   1. **"已受理"不等于"拍好了"**：后端 `snapshot.take` 是异步的（V4L2 出帧
   *      要几百毫秒），`queued:true` 只代表排上队了。所以点完按钮先显示"等待落盘"，
   *      **图片真的 load 出来**才说成功；load 不到就重试几次再报失败 ——
   *      板端踩过的正是"日志说 200、文件其实没写下来"。
   *   2. **列表来自 `GET /media/snap/`**（目录列表 JSON），刷新页面也能看到之前
   *      抓的图；否则用户会以为"抓完就丢了"。
   *   3. **不自动轮询列表**：T113 上一次轮询就是一次 SD 卡目录扫描 + 一串图片
   *      请求，何况也没人在盯着看。进页读一次、抓完刷一次、外加一个「刷新列表」。
   */
  function snapCard() {
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '抓拍（USB 摄像头）'));
    card.appendChild(el('p', 'hint',
      '摄像头插上 USB 口就能用，没有先后顺序（服务先起、摄像头后插也行；拔了再插也认）。' +
      '图片存 SD 卡 /mnt/sdcard/snap/（目录不存在会自动创建）。'));

    var bar = el('div', 'actions');
    var bShot = el('button', 'ghost', '立即抓拍');
    var bReload = el('button', 'ghost', '刷新列表');
    bShot.type = 'button';
    bReload.type = 'button';
    bar.appendChild(bShot);
    bar.appendChild(bReload);
    card.appendChild(bar);

    var msg = el('p', 'msg');
    card.appendChild(msg);

    /* 大图：只由 `loadBig()` 驱动。`pending` 是"正在等落盘的那张图"，
     * 为空串表示当前没有等待中的请求（onerror 里据此决定要不要重试）。 */
    var fig = el('div', 'snapfig hidden');
    var big = el('img', 'snapimg');
    var figCap = el('p', 'hint');
    big.alt = '抓拍图片';
    fig.appendChild(big);
    fig.appendChild(figCap);
    card.appendChild(fig);

    var grid = el('div', 'snapgrid');
    card.appendChild(grid);

    var base = '/media/snap/';   /* 列表返回 `d.dir` 后会被改写成本次真实目录 */
    var pending = '';
    var retry = 0;
    var afterShot = false;

    function fmtEpoch(t) {
      var d = new Date((t || 0) * 1000);
      function p2(n) { return (n < 10 ? '0' : '') + n; }
      return p2(d.getMonth() + 1) + '-' + p2(d.getDate()) + ' ' +
             p2(d.getHours()) + ':' + p2(d.getMinutes()) + ':' + p2(d.getSeconds());
    }

    big.onload = function () {
      if (pending === '') return;
      pending = '';
      retry = 0;
      fig.className = 'snapfig';
      bShot.disabled = false;
      msg.textContent = '';
      if (afterShot) {
        afterShot = false;
        toast('抓拍成功', 'ok');
        loadList();       /* 新图立刻进列表 */
      }
    };
    big.onerror = function () {
      if (pending === '') return;
      if (retry >= 8) {   /* 8 次 × 600 ms ≈ 5 s：够 V4L2 出帧 + 写 SD 卡了 */
        pending = '';
        afterShot = false;
        bShot.disabled = false;
        fig.className = 'snapfig hidden';
        msg.textContent = '抓拍失败：图片没有落盘 —— 摄像头没插好/被别的程序占用，' +
                          '或 SD 卡不可写（板端日志里能查到具体原因）。';
        toast('抓拍失败：图片没有落盘', 'err');
        return;
      }
      retry++;
      setTimeout(function () {
        if (pending === '') return;
        /* 加 ?t= 破缓存：同一秒重抓会命中同一个文件名 */
        big.src = pending + '?t=' + Date.now() + '&r=' + retry;
      }, 600);
    };

    function loadBig(url, isShot) {
      pending = url;
      retry = 0;
      afterShot = !!isShot;
      big.src = url + '?t=' + Date.now();
      fig.className = 'snapfig';
    }

    function loadList() {
      api('/media/snap/?n=24').then(function (r) {
        grid.innerHTML = '';
        if (!r.ok) {
          var why = (r.msg === 'not found' || r.msg === 'media root not available')
            ? '还没有抓拍图片（抓一张就会出现在这里）。'
            : ('图片列表读取失败：' + (r.msg || ('错误 ' + r.e)));
          grid.appendChild(el('p', 'hint', why));
          return;
        }
        var d = r.d || {};
        if (d.dir) {
          base = '/media' + d.dir;
          if (base.charAt(base.length - 1) !== '/') base += '/';
        }
        var items = (d.items || []).filter(function (it) {
          return /\.(jpg|jpeg|png)$/i.test(it.n || '');
        });
        if (items.length === 0) {
          grid.appendChild(el('p', 'hint', '还没有抓拍图片（抓一张就会出现在这里）。'));
          return;
        }
        /* 进页时把最新一张直接显示出来：不显示的话，"上次抓的那张在哪"没人答得上 */
        if (pending === '' && fig.className.indexOf('hidden') >= 0)
          loadBig(base + items[0].n, false);
        items.forEach(function (it) {
          var b = el('button', 'snapthumb');
          var im = el('img');
          b.type = 'button';
          im.src = base + it.n;
          im.alt = it.n;
          b.appendChild(im);
          b.appendChild(el('span', null, fmtEpoch(it.t)));
          b.addEventListener('click', function () {
            figCap.textContent = it.n + '（' + Math.round((it.s || 0) / 1024) + ' KB）';
            loadBig(base + it.n, false);
          });
          grid.appendChild(b);
        });
      }).catch(function () {
        grid.innerHTML = '';
        grid.appendChild(el('p', 'hint', '无法连接服务'));
      });
    }

    function shoot() {
      if (bShot.disabled) return;
      bShot.disabled = true;
      msg.textContent = '已提交抓拍，等待图片落盘…';
      api('/api/snapshot', { method: 'POST' }).then(function (r) {
        if (!r.ok) {
          bShot.disabled = false;
          /* `media not wired` 与录像卡同一句人话（两处各写一句迟早只改一处） */
          var why = (r.msg === 'media not wired')
            ? '本机固件未装配媒体链（ivsboxd 未打开媒体桥接）'
            : (r.msg || ('错误 ' + r.e));
          msg.textContent = '抓拍失败：' + why;
          toast('抓拍失败：' + why, 'err');
          return;
        }
        var d = r.d || {};
        if (!d.url) {
          /* 抓拍目录不在媒体根下（`--media-root` 与 snap_dir 不一致）：
           * 文件是写下来了，但本页面没法显示它 —— 如实说，别装成功。 */
          bShot.disabled = false;
          figCap.textContent = d.path || '';
          msg.textContent = '已抓拍，但图片不在媒体根下，页面无法显示：' + (d.path || '');
          loadList();
          return;
        }
        figCap.textContent = (d.name || '') + '（板端路径 ' + (d.path || '') + '）';
        loadBig(d.url, true);
      }).catch(function () {
        bShot.disabled = false;
        msg.textContent = '无法连接服务';
      });
    }

    bShot.addEventListener('click', shoot);
    bReload.addEventListener('click', loadList);
    loadList();

    return card;
  }

  /* 媒体（M6）：录像状态 + 手动启停。
   * 数据来自 `GET /api/media/record`（ctl 的 `record.page`，**一次读全 8 路**），
   * 启停走 `POST /api/media/record {"on":true|false}`（ctl 的 `record.manual`）。
   *
   * 两条不写在页面上的约定（写在这儿免得以后有人"顺手加一下"）：
   *   1. **不显示 RTSP 地址**：后端只回一个布尔（`url:true/false`），地址里有
   *      摄像机账密，页面没有理由拿到它；
   *   2. **失败原因照抄后端**：后端给的是"摄像机端口未开放：1.64:554 拒绝连接"
   *      这种能照着修的句子，页面一旦改写成"操作失败"就白做了（用户 2026-10-28
   *      明确要求：起不来要能看出是什么原因，方便排查）。 */
  function pageMedia() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '媒体'));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '录像'));
    var note = el('p', 'hint', '读取中…');
    card.appendChild(note);

    var bar = el('div', 'actions');
    var bOn = el('button', 'ghost', '立即录像');
    var bOff = el('button', 'ghost', '停止录像');
    bOn.type = 'button';
    bOff.type = 'button';
    bar.appendChild(bOn);
    bar.appendChild(bOff);
    card.appendChild(bar);

    /* 失败原因单独一行：**不能写进上面那句结论里** —— `load()` 每次都会重写
     * 结论，写在那儿会被 5 s 一次的自动刷新抹掉，用户恰好在这一刻抬头就什么
     * 都看不到。这一行只由动作结果写、由下一次动作清。 */
    var failLine = el('p', 'msg');
    card.appendChild(failLine);

    var box = el('div');
    card.appendChild(box);
    root.appendChild(card);

    /* 抓拍卡（USB 摄像头）：与录像卡是两条独立的路 —— 录像走网络 RTSP，
     * 抓拍走本机 V4L2，摄像头不在也不影响录像（反之亦然）。 */
    root.appendChild(snapCard());

    var tip = el('div', 'card');
    tip.appendChild(el('h3', null, '录像文件'));
    tip.appendChild(el('p', 'hint',
      '每路一个文件，按日期分目录：SD 卡 /mnt/sdcard/video/<yyyymmdd>/channel<路号>_<时分秒>.mp4，' +
      '每段 15 分钟自动切分。SD 卡未挂载或写保护时不会开新录像（水位 ≥90% 不新开、≥95% 立即停）。'));
    root.appendChild(tip);

    var busy = false;

    function act(on) {
      if (busy) return;
      busy = true;
      bOn.disabled = true;
      bOff.disabled = true;
      failLine.textContent = '';
      note.textContent = on ? '正在启动录像…' : '正在停止录像…';
      api('/api/media/record', { method: 'POST', body: { on: on } })
        .then(function (r) {
          busy = false;
          bOn.disabled = false;
          bOff.disabled = false;
          if (!r.ok) {
            var why = r.msg || ('错误 ' + r.e);
            toast((on ? '启动失败：' : '停止失败：') + why, 'err');
            failLine.textContent = (on ? '启动失败：' : '停止失败：') + why;
            load();
            return;
          }
          var changed = !!(r.d && r.d.changed);
          toast(on ? (changed ? '已开始录像' : '本来就在录')
                   : (changed ? '已停止录像' : '本来就没在录'), 'ok');
          load();
        })
        .catch(function () {
          busy = false;
          bOn.disabled = false;
          bOff.disabled = false;
          failLine.textContent = '无法连接服务';
          note.textContent = '';
        });
    }
    bOn.addEventListener('click', function () { act(true); });
    bOff.addEventListener('click', function () { act(false); });

    /* 一路的"状态"三态：配了才能录、开了才会录、在录才算录。
     * 三者的文字要能区分 —— 都写成"未录"的话，现场分不清该去改哪一处。 */
    function chState(ch) {
      if (!ch.url) return ['未配置地址', 'badge'];
      if (!ch.want) return ['录像开关关闭', 'badge'];
      if (ch.failed) return ['无法启动', 'badge err'];
      if (ch.on) return ['录制中', 'badge ok'];
      return ['未录', 'badge'];
    }

    function load() {
      api('/api/media/record').then(function (r) {
        box.innerHTML = '';
        if (!r.ok) {
          note.textContent = '';
          var why = (r.msg === 'media not wired')
            ? '本机固件未装配媒体链（ivsboxd 未打开媒体桥接）'
            : (r.msg || '读取失败');
          box.appendChild(kv('状态', why, 'err'));
          return;
        }
        var d = r.d || {}, chs = d.ch || [];

        /* 一句话结论：**自动** / **手动** / **人工停过** 三种必须分开说 ——
         * 手动态主网恢复也不会停；而"人工停过"时主网断着却不录，不说清楚
         * 现场只会当成功能坏了（2026-10-28 板端就是这个形态）。 */
        if (d.recording) {
          note.textContent = d.forced
            ? '正在录像（手动启动：不受主网状态影响，需点「停止录像」才会停）'
            : '正在录像（主网断开触发：主网恢复一段时间后会自动停止）';
        } else if (d.held_off) {
          note.textContent = '已人工停止录像：暂时压制了「主网断开自动录像」，' +
            '主网恢复一次后自动解除。要马上录请点「立即录像」。';
        } else {
          note.textContent = '当前未录像 —— 主网断开并保持一段时间后自动开始；' +
            '也可以点「立即录像」马上开始（不受主网状态限制）。';
        }

        var t = el('table');
        var head = el('tr');
        ['通道', '录像开关', '拉流地址', '状态', '未启动原因'].forEach(function (h) {
          head.appendChild(el('th', null, h));
        });
        t.appendChild(head);
        chs.forEach(function (ch) {
          var tr = el('tr');
          tr.appendChild(el('td', null, '第 ' + (ch.id + 1) + ' 路'));
          /* 开关与地址都只读：改它们去「摄像机」页（一处写者，改完即时热生效） */
          tr.appendChild(el('td', null, ch.want ? '开' : '关'));
          tr.appendChild(el('td', null, ch.url ? '已配置' : '未配置'));
          var st = chState(ch);
          var td = el('td');
          td.appendChild(el('span', st[1], st[0]));
          tr.appendChild(td);
          tr.appendChild(el('td', null, ch.why || '—'));
          t.appendChild(tr);
        });
        box.appendChild(t);
      }).catch(function () {
        box.innerHTML = '';
        note.textContent = '';
        box.appendChild(kv('状态', '无法连接服务', 'err'));
      });
    }

    load();
    st.timer = setInterval(load, 5000);  /* 5 s 自动刷新；切页时由 go() 收掉 */
    return root;
  }

  function pageConfig() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '全部配置'));
    /* `camera.ch.*.mac` 不给入口（2026-10-27 用户口径）：MAC 是摄像机发现阶段
     * 自己拿到的档案字段，不是现场该手填的配置。键仍留在默认值表里兼容老配置，
     * 只是两处界面都不再画它（摄像机页连整片 `camera.ch.*` 都交给专用卡片）。 */
    root.appendChild(configCard('八个一级分类（全量）',
      ['sys', 'net', 'probe', 'camera', 'switch', 'elec', 'netthr', 'sensor'],
      '这一页是原始视图（含未归入专用页的阈值项）。改动后保存会带版本号提交。' +
      '摄像机 MAC 由发现过程自动获取，此处不提供配置项。',
      null, ['camera.ch.*.mac']));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '出厂默认值对照'));
    var box = el('div');
    card.appendChild(box);
    root.appendChild(card);
    api('/api/config?defaults=1').then(function (r) {
      if (!r.ok) { box.appendChild(el('p', 'msg', r.msg || '读取失败')); return; }
      var keys = Object.keys(r.d.items).sort();
      var t = el('table');
      var head = el('tr');
      ['配置键', '类型', '出厂默认'].forEach(function (h) { head.appendChild(el('th', null, h)); });
      t.appendChild(head);
      keys.forEach(function (k) {
        var tr = el('tr');
        tr.appendChild(el('td', null, k));
        tr.appendChild(el('td', null, r.d.items[k].t));
        tr.appendChild(el('td', null, String(r.d.items[k].v)));
        t.appendChild(tr);
      });
      box.appendChild(t);
    });
    return root;
  }

  function pageMaint() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '运维'));

    var ops = el('div', 'card');
    ops.appendChild(el('h3', null, '系统操作'));
    var bar = el('div', 'actions');

    var bSync = el('button', 'ghost', '同步本机时间');
    bSync.type = 'button';
    bSync.addEventListener('click', function () {
      var ts = Math.floor(Date.now() / 1000);
      api('/api/system/time', { method: 'POST', body: { ts: ts } }).then(function (r) {
        toast(r.ok ? '已同步为浏览器时间' : ('同步失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
      });
    });
    bar.appendChild(bSync);

    var bSearch = el('button', 'ghost', '搜索摄像机');
    bSearch.type = 'button';
    bSearch.addEventListener('click', function () {
      api('/api/camera/search', { method: 'POST', body: {} }).then(function (r) {
        toast(r.ok ? '已触发搜索' : ('失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
      });
    });
    bar.appendChild(bSearch);

    var bReboot = el('button', 'danger', '重启设备');
    bReboot.type = 'button';
    bReboot.addEventListener('click', function () {
      if (!window.confirm('确认重启设备？现场业务会中断。')) return;
      api('/api/system/reboot', { method: 'POST', body: { confirm: true } }).then(function (r) {
        toast(r.ok ? '已下发重启指令' : ('失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
      });
    });
    bar.appendChild(bReboot);

    ops.appendChild(bar);
    root.appendChild(ops);

    /* ---- 恢复出厂 / 擦除存储（2026-10-22 单片机 web 对齐第 ⑥ 项）----
     * 三条口径（都拍过板，别在这里"顺手优化"）：
     *   1. **擦除存储是独立勾选、默认不勾**。录像/快照、日志、数据库历史删了就
     *      没了，不能跟"恢复出厂"绑着默认执行。不勾时请求体里**根本没有 `erase`
     *      键**，服务端按"只恢复配置"处理（见 web/src/iv_web_api.c 的
     *      api_factory_reset 与 iv_factory.h 的三条口径）。
     *   2. **口令回出厂默认并强制改密**。这一步由 ivsbox-web 自己做 —— 它是
     *      `<secure>/web.passwd` 的唯一写者，ivsboxd 不认识这个路径。成功后全部
     *      会话作废，所以本页必须回登录页，不能让用户对着已经失效的界面点按钮。
     *   3. **不自动重启**。配置已落盘、各模块已按新配置跑起来了，重启只会多断
     *      一次现场业务；真要重启，上面那颗"重启设备"是独立按钮。 */
    var fac = el('div', 'card');
    fac.appendChild(el('h3', null, '恢复出厂'));
    fac.appendChild(el('p', 'hint',
      '把配置全部恢复为出厂默认值（工厂身份 devid / MAC 在独立分区，不受影响）。' +
      '执行后登录口令回到出厂默认并强制改密，当前登录会被踢出。'));
    var facChk = el('label', 'field chk');
    var facErase = document.createElement('input');
    facErase.type = 'checkbox';
    facErase.id = 'fac-erase';
    facChk.appendChild(facErase);
    facChk.appendChild(el('span', null, '同时擦除存储（录像/快照、按小时日志、数据库历史记录）'));
    fac.appendChild(facChk);
    fac.appendChild(el('p', 'hint', '擦除不可撤销：不勾这一项就只恢复配置，录像与日志一条都不动。'));

    var facAct = el('div', 'actions');
    var bFac = el('button', 'danger', '恢复出厂设置');
    bFac.type = 'button';
    var facBusy = false;
    bFac.addEventListener('click', function () {
      if (facBusy) return; /* 防重入：连点两下会下发两次，第二次擦的是空气但配置会再存一遍 */
      var erase = !!facErase.checked;
      if (!window.confirm(erase
        ? '确认恢复出厂，并擦除全部录像/快照、日志与数据库历史？'
        : '确认恢复出厂？配置将全部回到出厂默认值。'))
        return;
      /* 勾了擦除再问一次：这一步删的是现场数据，"不可撤销"要用户亲手点第二遍 */
      if (erase && !window.confirm('第二步确认：勾选的存储数据会被永久删除，无法恢复。仍要继续？'))
        return;
      var body = { confirm: true };
      if (erase) body.erase = ['media', 'log', 'db'];
      facBusy = true;
      bFac.disabled = true;
      api('/api/factory_reset', { method: 'POST', body: body }).then(function (r) {
        facBusy = false;
        bFac.disabled = false;
        if (!r.ok) {
          toast('恢复出厂失败：' + (r.msg || r.e), 'err');
          return;
        }
        var d = r.d || {};
        var m = '配置 ' + ((d.cfg_changed !== undefined) ? (d.cfg_changed + ' 项回默认') : '已恢复默认');
        if (erase) {
          m += '，删除 ' + (d.entries_removed || 0) + ' 个文件/目录、' +
               (d.db_rows_removed || 0) + ' 行数据库记录';
        }
        /* 口令已回出厂 ⇒ 会话全废，回登录页（提示语里给出出厂口令，现场要能照着重登）。
         * 注意 `password_reset` 在**顶层**（ivsbox-web 自己加的），不在 d 里 ——
         * d 是 ctl 原样透传的那一份，别在 d 上找它。 */
        lastPass = '';
        showLogin('恢复出厂完成（' + m + '）。请用 ' +
                  (r.password_reset ? '出厂口令重新登录并立即改密' : '原口令重新登录') + '。');
      }).catch(function () {
        facBusy = false;
        bFac.disabled = false;
        toast('恢复出厂失败：无法连接服务', 'err');
      });
    });
    facAct.appendChild(bFac);
    fac.appendChild(facAct);
    root.appendChild(fac);

    /* ---- 系统升级（状态只读 ＋ 包上传 ＋ 安装请求）----
     * 参考 web 里的「升级有线/升级无线」是"设备自己去服务器下载"，本机没有下载器：
     * 包由浏览器（或平台/U 盘）送到设备上。分工写死在这里：
     *   本页只做「把包传上去 + 落一个安装请求」；**真正改盘安装由 monitor.sh 拉起
     *   ivsbox-updater 执行**（单写者原则，见 iv_upd.h）。所以状态区永远只读，
     *   装没装成看 `ota.status` 的 state/最近事件，不是看本页的按钮。 */
    var up = el('div', 'card');
    up.appendChild(el('h3', null, '系统升级'));
    var upHint = el('p', 'hint', '读取中…');
    up.appendChild(upHint);
    var upGrid = el('div', 'grid');
    up.appendChild(upGrid);

    /* ---- 上传升级包（分片）----
     * 为什么非分片不可：`IV_WEB_BODY_MAX` 是 32 KiB，而版本包有几 MB —— 一次 POST 根本
     * 传不进来。每片 24 KiB（与服务端 `IV_WEB_OTA_SLICE` 是同一个数，改要同时改），
     * 断了能从续传点接上，不必从头再来（现场多半是 4G）。
     * 传完只发**安装请求**：真正改盘由 monitor.sh 拉起 ivsbox-updater 执行（单写者），
     * 所以按钮文案是"上传并请求安装"，不是"立即升级"。
     * 浏览器侧的 SHA-256 在纯 http（非 localhost）下拿不到 `crypto.subtle`，故摘要交给
     * 服务端算；完整性另有安装时的 Ed25519 验签兜底。 */
    var OTA_SLICE = 24 * 1024;
    var OTA_MAX = 64 * 1024 * 1024;
    var otaFile = null;
    var otaBusy = false;
    var otaCancel = false;

    var upPick = el('div', 'actions');
    var fUp = document.createElement('input');
    fUp.type = 'file';
    fUp.id = 'ota-file';
    upPick.appendChild(fUp);
    up.appendChild(upPick);

    var upFileInfo = el('p', 'hint', '未选择升级包');
    up.appendChild(upFileInfo);
    var upProg = el('p', 'hint', '');
    up.appendChild(upProg);

    var upAct = el('div', 'actions');
    var bUpSend = el('button', 'ghost', '上传并请求安装');
    bUpSend.type = 'button';
    bUpSend.disabled = true;
    var bUpAbort = el('button', 'ghost', '取消上传');
    bUpAbort.type = 'button';
    bUpAbort.disabled = true;
    upAct.appendChild(bUpSend);
    upAct.appendChild(bUpAbort);
    up.appendChild(upAct);

    fUp.addEventListener('change', function () {
      otaFile = (fUp.files && fUp.files[0]) || null;
      bUpSend.disabled = !otaFile || otaBusy;
      upFileInfo.textContent = otaFile
        ? ('已选：' + (otaFile.name || '(无名)') + '（' + fmtSize(otaFile.size) + '）')
        : '未选择升级包';
      upProg.textContent = '';
    });

    /* 包名只留安全字符：服务端按正向白名单判（`[A-Za-z0-9][A-Za-z0-9._-]*`），
     * 这里先就地改一遍，免得用户选了个中文名/带空格的名字被拒还不知道为什么。 */
    function otaSafeName(n) {
      return String(n || 'pkg').replace(/[^A-Za-z0-9._-]/g, '_').replace(/^\./, '_');
    }

    /* 分片体是二进制，走不了 api() 那套 JSON 封装，单独一条 fetch。 */
    function otaChunk(name, off, blob) {
      return fetch('/api/ota/chunk?name=' + encodeURIComponent(name) + '&off=' + off, {
        method: 'POST',
        credentials: 'same-origin',
        body: blob,
        headers: { 'Content-Type': 'application/octet-stream' }
      }).then(function (r) {
        return r.text().then(function (t) {
          var j;
          try { j = JSON.parse(t); } catch (e) { j = { ok: false, e: -1, msg: '响应不是 JSON' }; }
          j._status = r.status;
          return j;
        });
      });
    }

    function otaSettle(msg, kind) {
      otaBusy = false;
      bUpSend.disabled = !otaFile;
      bUpAbort.disabled = true;
      if (msg) {
        upProg.textContent = msg;
        toast(msg, kind || 'err');
      }
    }

    function uploadFrom(name, off) {
      if (otaCancel) return Promise.resolve(otaSettle('已取消上传'));
      if (off >= otaFile.size) return otaFinish(name);
      var end = Math.min(off + OTA_SLICE, otaFile.size);
      var blob = otaFile.slice(off, end);
      return otaChunk(name, off, blob).then(function (r) {
        /* 服务端说偏移对不上：按它给的实际值接（它才是真相，别自己猜） */
        if (r._status === 409 && r.d && typeof r.d.off === 'number') {
          upProg.textContent = '偏移对不上，按服务端的 ' + fmtSize(r.d.off) + ' 续传…';
          return uploadFrom(name, r.d.off);
        }
        if (!r.ok) throw new Error(r.msg || ('分片失败 e=' + r.e));
        var done = (r.d && r.d.next) || end;
        var pct = otaFile.size ? Math.min(100, Math.round(done * 100 / otaFile.size)) : 100;
        upProg.textContent = '已上传 ' + fmtSize(done) + ' / ' + fmtSize(otaFile.size) +
          '（' + pct + '%）';
        return uploadFrom(name, done);
      });
    }

    function otaFinish(name) {
      upProg.textContent = '校验并落位…';
      return api('/api/ota/finish', { method: 'POST', body: { name: name, size: otaFile.size } })
        .then(function (r) {
          if (!r.ok) {
            var d = r.d || {};
            var extra = (d.got != null) ? ('（服务端收到 ' + fmtSize(d.got) + '）') : '';
            return otaSettle('包校验失败：' + (r.msg || r.e) + extra);
          }
          var ok = r.d || {};
          upProg.textContent = '包已就位（' + fmtSize(ok.size) + '），摘要 ' + (ok.sha256 || '—') +
            '，正在请求安装…';
          return api('/api/ota/install', { method: 'POST', body: { name: name, restart: 1 } })
            .then(function (ir) {
              if (!ir.ok) return otaSettle('安装请求失败：' + (ir.msg || ir.e));
              otaSettle('已请求安装：由 monitor.sh 拉起 ivsbox-updater 执行，进度看上面的状态',
                'ok');
              setTimeout(loadOta, 1500);
            });
        })
        .catch(function (e) {
          otaSettle('上传中断：' + (e && e.message ? e.message : e) + '（可重试，会从续传点接上）');
        });
    }

    bUpSend.addEventListener('click', function () {
      if (otaBusy || !otaFile) return;
      if (!otaFile.size) { toast('空文件不能作为升级包', 'err'); return; }
      if (otaFile.size > OTA_MAX) {
        toast('包超过上限 ' + fmtSize(OTA_MAX), 'err');
        return;
      }
      var name = otaSafeName(otaFile.name);
      if (name !== otaFile.name) toast('包名含特殊字符，按 ' + name + ' 保存', 'warn');
      otaBusy = true;
      otaCancel = false;
      bUpSend.disabled = true;
      bUpAbort.disabled = false;
      upProg.textContent = '查询续传点…';
      api('/api/ota/chunk?name=' + encodeURIComponent(name)).then(function (r) {
        var off = (r.ok && r.d && typeof r.d.off === 'number') ? r.d.off : 0;
        if (off >= otaFile.size) off = 0;  /* 服务端那份不比当前文件小 ⇒ 从头传 */
        if (off > 0) upProg.textContent = '检测到未完成的上传，从 ' + fmtSize(off) + ' 续传…';
        return uploadFrom(name, off);
      }).catch(function (e) {
        otaSettle('续传点查询失败：' + (e && e.message ? e.message : e));
      });
    });

    bUpAbort.addEventListener('click', function () {
      if (!otaBusy) return;
      otaCancel = true;
      bUpAbort.disabled = true;
      upProg.textContent = '正在取消…';
      if (otaFile) {
        api('/api/ota/abort', {
          method: 'POST',
          body: { name: otaSafeName(otaFile.name) }
        }).then(function () { otaSettle('已取消，未完成的分片已丢弃'); });
      } else {
        otaSettle('已取消');
      }
    });

    var upBar = el('div', 'actions');
    var bUpRefresh = el('button', 'ghost', '刷新升级状态');
    bUpRefresh.type = 'button';
    bUpRefresh.addEventListener('click', function () { loadOta(); });
    upBar.appendChild(bUpRefresh);
    up.appendChild(upBar);
    root.appendChild(up);

    function loadOta() {
      upHint.textContent = '读取中…';
      api('/api/ota').then(function (r) {
        upGrid.textContent = '';
        if (!r.ok) {
          upHint.textContent = '升级状态不可用：' + (r.msg || r.e);
          return;
        }
        var d = r.d || {};
        upHint.textContent = d.pending
          ? '有已落地的包，等待 ivsbox-updater 安装'
          : '当前没有待安装的包';
        upGrid.appendChild(kv('当前版本', fmtVer(d.version)));
        upGrid.appendChild(kv('上一版本', fmtVer(d.previous)));
        upGrid.appendChild(kv('运行分区', d.current || '—'));
        upGrid.appendChild(kv('状态', d.state || '—'));
        upGrid.appendChild(kv('待安装', d.pending ? '是' : '否', d.pending ? 'warn' : null));
        upGrid.appendChild(kv('启动计数', (d.bootcount || 0) + ' / ' + (d.max_boot || 0),
          (d.bootcount || 0) > 1 ? 'warn' : null));
        upGrid.appendChild(kv('回滚次数', d.rollbacks || 0, d.rollbacks ? 'warn' : null));
        upGrid.appendChild(kv('最近事件', d.last_event || '—'));
        upGrid.appendChild(kv('最近原因', d.last_reason || '—'));
      });
    }
    loadOta();

    /* ---- 系统日志（对齐参考 web 的「打印日志」）----
     * 列表与下载都由 ivsbox-web 直接读 `/mnt/UDISK/log/<日期>/<小时>.log`：
     * 日志不是 ivsboxd 的内部状态，为一句话的目录列表单开一条协议不划算。 */
    var lg = el('div', 'card');
    lg.appendChild(el('h3', null, '系统日志'));
    var lgHint = el('p', 'hint', '读取中…');
    lg.appendChild(lgHint);
    var lgList = el('div', 'grid');
    lg.appendChild(lgList);
    var lgBar = el('div', 'actions');
    var bLgRefresh = el('button', 'ghost', '刷新日志列表');
    bLgRefresh.type = 'button';
    bLgRefresh.addEventListener('click', function () { loadLogs(); });
    lgBar.appendChild(bLgRefresh);
    lg.appendChild(lgBar);
    root.appendChild(lg);

    function fmtSize(n) {
      n = Number(n) || 0;
      if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
      if (n >= 1024) return Math.round(n / 1024) + ' KB';
      return n + ' B';
    }

    function loadLogs() {
      lgHint.textContent = '读取中…';
      api('/api/logs?max=20').then(function (r) {
        lgList.textContent = '';
        if (!r.ok) {
          lgHint.textContent = '日志列表不可用：' + (r.msg || r.e);
          return;
        }
        var d = r.d || {};
        var files = d.files || [];
        if (!files.length) {
          lgHint.textContent = '没有日志文件（目录 ' + (d.root || '?') + '）。';
          return;
        }
        lgHint.textContent = (d.root || '') + ' —— 共 ' + (d.total || 0) +
          ' 个文件，下面是最近 ' + files.length + ' 个。';
        files.forEach(function (f) {
          var row = el('div', 'kv');
          row.appendChild(el('div', 'k', f.f));
          var v = el('div', 'v');
          v.appendChild(document.createTextNode(fmtSize(f.size) + ' · ' + fmtWall(f.mtime) + '　'));
          var a = el('a', null, '下载');
          a.href = '/api/logs/file?f=' + encodeURIComponent(f.f);
          a.setAttribute('download', '');
          v.appendChild(a);
          row.appendChild(v);
          lgList.appendChild(row);
        });
      });
    }
    loadLogs();

    root.appendChild(configCard('检测与自愈（probe.* / netthr.*）', ['probe', 'netthr'],
      '探活节拍、连续失败次数、故障重启次数与冷却。主网检测 IP 1/2 与有线网络超时时间'
      + '（`probe.timeout_ms`，即**延时门限**）已收进「网络配置」页的「本机网络与主网检测」卡片'
      + '（与平台摆在一起），这里不再重复。',
      null, ['probe.target1', 'probe.target2', 'probe.timeout_ms']));
    root.appendChild(configCard('电阈值与传感器（elec.* / sensor.*）', ['elec', 'sensor'],
      '上报判据用的物理量阈值。'));
    return root;
  }

  /* ==================== 启动 ==================== */
  function boot() {
    /* 规则原文一次填好（用 <pre> 才保得住换行） */
    $('#passRule').textContent = PASS_HINT;

    $('#loginForm').addEventListener('submit', function (ev) {
      ev.preventDefault();
      tryLogin($('#user').value, $('#pass').value);
    });
    $('#chpassForm').addEventListener('submit', function (ev) {
      ev.preventDefault();
      changePass(chpassForced ? lastPass : $('#oldpass').value,
                 $('#newpass').value, $('#newpass2').value);
    });
    /* 侧栏"改密"：主动改密（要原密码，可返回） */
    $('#chpassBtn').addEventListener('click', function () { showChpass('', false); });
    $('#chpassCancel').addEventListener('click', function () {
      $('#chpass').className = 'login hidden';
      $('#app').className = 'app';
    });
    $('#logoutBtn').addEventListener('click', function () {
      api('/api/logout', { method: 'POST', body: {} }).then(function () {
        lastPass = '';
        showLogin('已退出');
      });
    });

    /* 探测会话是否还在：随便取一个需要鉴权的接口，401 就回登录页。
     * 403（出厂默认密码还没改）不在这里另作处理 —— api() 已经切到改密卡片了。 */
    api('/api/overview').then(function (r) {
      if (r._status === 401) showLogin('');
      else if (r.ok) { st.version = (r.d && r.d.version) || ''; $('#verLabel').textContent = st.version; showApp(); }
      else if (r._status !== 403) showLogin('');
    }).catch(function () { showLogin('无法连接服务'); });
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', boot);
  else
    boot();
})();
