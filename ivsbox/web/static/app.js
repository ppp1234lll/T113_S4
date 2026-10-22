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
  var MN_NAME = { 0: '未判定', 1: '正常', 2: '断开' };
  var SNMP_STATE = { 0: '空闲', 1: '采集中', 2: '就绪' };

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

  function fieldFor(key, val, desc) {
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
    return lab;
  }

  function collectChanges(scope) {
    var items = {};
    var inputs = scope.querySelectorAll('[data-key]');
    for (var i = 0; i < inputs.length; i++) {
      var inp = inputs[i], kind = inp.dataset.kind, cur, orig = JSON.parse(inp.dataset.orig);
      if (kind === 'boolean') cur = inp.checked;
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
   * 得靠调用方自己补一个入口 —— 回调就是为了把 items 与 reload 交出去。 */
  function configCard(title, classes, note, onReady) {
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
  var CD_MODE = { 1: 'PING', 2: '端口扫描', 3: 'RTSP OPTIONS' };

  var NET_STATE = {
    WIRELESS_UP: '无线已连接',
    WIRELESS_DEGRADED: '无线信号差',
    SWITCH_TO_WIRED: '正在切到有线',
    WIRED_UP: '有线已连接',
    SWITCH_TO_WIRELESS: '正在切到无线',
    BOTH_DOWN: '双链路断开'
  };

  /* 构建时没注入 git 元数据，iv_version 会拼出 "1.0.0+gunknown"。
   * 原样显示容易被当成版本号写错，这里点明是开发构建。 */
  function fmtVer(v) {
    if (!v) return '—';
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
        grid.appendChild(kv('软件版本', fmtVer(dv.version || d.version)));
        grid.appendChild(kv('硬件型号', dv.model || '—'));
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
          lGrid.appendChild(kv('主网1', MN_NAME[nt.mn[0]] || '—',
            nt.mn[0] === 2 ? 'err' : (nt.mn[0] === 1 ? 'ok' : 'warn')));
          lGrid.appendChild(kv('主网2', MN_NAME[nt.mn[1]] || '—',
            nt.mn[1] === 2 ? 'err' : (nt.mn[1] === 1 ? 'ok' : 'warn')));
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

    root.appendChild(configCard('网络参数（net.*）', ['net', 'sys'],
      '含内外网平台地址、无线/有线网口与传输模式。' +
      '本机 IP / 掩码 / 网关留空表示"不在此处指定"，实际生效值见上方「当前生效」。'));
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

  function pageCamera() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '摄像机'));

    /* 六路 CNS 与检测方式原先在总览页，现归本页：状态和"这一路配的是哪个 IP"
     * 放在一起才有用 —— 看到"3 号故障"紧接着就能核对它的 IP/账号。
     * CNS 口径：0 无IP / 1 正常 / 2 故障 / 4 延时 / 5 丢包（无 3）。 */
    var card = el('div', 'card');
    card.appendChild(el('h3', null, '摄像机状态（实时）'));
    var hint = el('p', 'hint', '读取中…');
    card.appendChild(hint);
    var grid = el('div', 'grid');
    card.appendChild(grid);
    root.appendChild(card);

    api('/api/overview').then(function (r) {
      var cm = (r && r.ok && r.d) ? r.d.cameras : null;

      if (!cm || !cm.cns || !cm.cns.length) {
        hint.textContent = '未获取到摄像机状态（检测模块未装配或尚未开始检测）。';
        return;
      }
      hint.textContent = '检测方式：' + (cm.mode ? (CD_MODE[cm.mode] || cm.mode) : '—') +
        '　判定口径：0 无IP／1 正常／2 故障／4 延时／5 丢包。';
      for (var i = 0; i < cm.cns.length; i++)
        grid.appendChild(kv('摄像机' + (i + 1),
          CNS_NAME[cm.cns[i]] === undefined ? cm.cns[i] : CNS_NAME[cm.cns[i]],
          cm.cns[i] === 1 ? 'ok' : (cm.cns[i] === 2 ? 'err' : 'warn')));
    });

    var slot = cameraSlotCard();
    root.appendChild(configCard('摄像机参数与账号（camera.*）', ['camera'],
      '检测方式、6 路 IP/账号/端口，以及 SNMP OID 槽位（按机型填值）。',
      function (items, reload) { slot.sync(items, reload); }));
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

    var card = el('div', 'card');
    card.appendChild(el('h3', null, 'SNMP 采集结果'));
    var bar = el('div', 'actions');
    var bCam = el('button', null, '采集摄像机');
    var bSw = el('button', null, '采集交换机');
    bCam.type = 'button'; bSw.type = 'button';
    bar.appendChild(bCam); bar.appendChild(bSw);
    card.appendChild(bar);
    var box = el('div');
    card.appendChild(box);

    function show(which) {
      box.innerHTML = '读取中…';
      api('/api/snmp?which=' + which).then(function (r) {
        box.innerHTML = '';
        if (!r.ok) { box.appendChild(el('p', 'msg', r.msg || '读取失败')); return; }
        box.appendChild(el('p', 'hint', '状态：' + (SNMP_STATE[r.d.state] || r.d.state)));
        if (!r.d.slots.length) { box.appendChild(el('p', 'hint', '尚无采集结果')); return; }
        var t = el('table');
        var head = el('tr');
        ['槽位', 'OID', '行数', '取值'].forEach(function (h) { head.appendChild(el('th', null, h)); });
        t.appendChild(head);
        r.d.slots.forEach(function (s) {
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
    function run(which) {
      api('/api/snmp/run', { method: 'POST', body: { which: which } }).then(function (r) {
        toast(r.ok ? '已发起采集' : ('失败：' + (r.msg || r.e)), r.ok ? 'ok' : 'err');
        if (r.ok) setTimeout(function () { show(which); }, 2500);
      });
    }
    bCam.addEventListener('click', function () { run('cam'); });
    bSw.addEventListener('click', function () { run('sw'); });
    show('cam');
    root.appendChild(card);
    return root;
  }

  function pageMedia() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '媒体'));
    var card = el('div', 'card');
    card.appendChild(el('p', 'hint', '媒体功能（抓图 / 录像 / 视频读取）属 M6，尚未启用。'));
    root.appendChild(card);
    return root;
  }

  function pageConfig() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '全部配置'));
    root.appendChild(configCard('八个一级分类（全量）',
      ['sys', 'net', 'probe', 'camera', 'switch', 'elec', 'netthr', 'sensor'],
      '这一页是原始视图（含未归入专用页的阈值项）。改动后保存会带版本号提交。'));

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
      '探活目标与节拍、故障重启次数与冷却。'));
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
