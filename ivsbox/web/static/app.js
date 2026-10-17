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
    { id: 'gps', name: '定位' },
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
      overview: pageOverview, net: pageNet, gps: pageGps, camera: pageCamera,
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

  /* 拉一个分类的配置，渲染成「分组 → 表单」。classes 可以是数组。 */
  function configCard(title, classes, note) {
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
  function pageOverview() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '总览'));

    var card = el('div', 'card');
    card.appendChild(el('h3', null, '设备状态'));
    var grid = el('div', 'grid');
    card.appendChild(grid);
    root.appendChild(card);

    var up = el('div', 'card');
    up.appendChild(el('h3', null, '采集板镜像（0xE1）'));
    var ugrid = el('div', 'grid');
    up.appendChild(ugrid);
    root.appendChild(up);

    api('/api/overview').then(function (r) {
      if (!r.ok) { toast(r.msg || '总览读取失败', 'err'); return; }
      var d = r.d;
      st.version = d.version || '';
      $('#verLabel').textContent = st.version;
      grid.appendChild(kv('软件版本', d.version));
      grid.appendChild(kv('运行时长', fmtUptime(d.uptime_ms)));
      grid.appendChild(kv('网络状态', (d.net && d.net.state) || '-'));
      grid.appendChild(kv('活动出口', (d.net && d.net.active_wan === 'wired') ? '有线' : '无线'));
      grid.appendChild(kv('发现设备', d.devices + ' 台'));
      grid.appendChild(kv('门开事件', d.door_events + ' 次'));
      if (d.net && d.net.mn) {
        grid.appendChild(kv('主网1', MN_NAME[d.net.mn[0]] || '-'));
        grid.appendChild(kv('主网2', MN_NAME[d.net.mn[1]] || '-'));
      }
      if (d.cameras && d.cameras.cns) {
        for (var i = 0; i < d.cameras.cns.length; i++)
          grid.appendChild(kv('摄像机' + (i + 1), CNS_NAME[d.cameras.cns[i]] || d.cameras.cns[i],
            d.cameras.cns[i] === 1 ? 'ok' : (d.cameras.cns[i] === 2 ? 'err' : 'warn')));
      }
      if (d.snmp) {
        grid.appendChild(kv('SNMP 摄像机', SNMP_STATE[d.snmp.cam] || d.snmp.cam));
        grid.appendChild(kv('SNMP 交换机', SNMP_STATE[d.snmp.sw] || d.snmp.sw));
      }
      if (d.status) {
        ugrid.appendChild(kv('电压', d.status.V === null ? '—' : d.status.V + ' V'));
        ugrid.appendChild(kv('电流', d.status.A === null ? '—' : d.status.A + ' A'));
        ugrid.appendChild(kv('温度', d.status.T === null ? '—' : d.status.T + ' ℃'));
        ugrid.appendChild(kv('湿度', d.status.H === null ? '—' : d.status.H + ' %'));
        ugrid.appendChild(kv('箱门', d.status.DS === null ? '—' : (d.status.DS === 2 ? '开' : '关')));
      }
      if (d.power) {
        ugrid.appendChild(kv('总功率', d.power.APOWER || '—'));
        ugrid.appendChild(kv('总电量', d.power.AKW || '—'));
      }
    });
    return root;
  }

  function pageNet() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '网络配置'));

    /* 「当前生效」：配置里的 net.ip / net.mask / net.gateway 留空＝"不在此指定，
     * 交给系统网络配置决定"（本产品实际由天嵌 EmbedSkyProgress.ini 生效）。
     * 光看三个空框根本看不出设备现在是什么地址，所以单列一块**实际值**（只读，
     * 数据来自内核：/api/overview 的 d.net.link）。 */
    var live = el('div', 'card');
    live.appendChild(el('h3', null, '当前生效（只读）'));
    var hint = el('p', 'hint', '读取中…');
    live.appendChild(hint);
    var grid = el('div', 'grid');
    live.appendChild(grid);
    root.appendChild(live);

    api('/api/overview').then(function (r) {
      var lk = (r && r.ok && r.d && r.d.net) ? r.d.net.link : null;
      hint.textContent = '以下是设备当前的真实地址（取自内核），与下方配置项里填的值无关。';
      if (!lk) { grid.appendChild(kv('状态', '未获取到网口信息', 'warn')); return; }
      var NAMES = { eth0: '有线网口 eth0', usb0: '无线网口 usb0' };
      (lk.ifaces || []).forEach(function (f) {
        var nm = NAMES[f.name] || f.name;
        var st, cls;
        if (!f.present) { st = '未检测到'; cls = 'warn'; }
        else if (!f.up) { st = '未启用'; cls = 'warn'; }
        else if (!f.ip) { st = '已启用，无 IPv4 地址'; cls = 'warn'; }
        else { st = '已连接'; cls = 'ok'; }
        grid.appendChild(kv(nm, st + (f.ip ? '　' + f.ip : ''), cls));
        grid.appendChild(kv(nm + ' · 子网掩码', f.mask || '—'));
        grid.appendChild(kv(nm + ' · MAC', f.mac || '—'));
      });
      grid.appendChild(kv('默认网关',
        (lk.gateway || '无') + (lk.gateway_if ? '（' + lk.gateway_if + '）' : '')));
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

  function pageCamera() {
    var root = document.createDocumentFragment();
    root.appendChild(el('h2', 'page-title', '摄像机'));
    root.appendChild(configCard('摄像机参数与账号（camera.*）', ['camera'],
      '检测方式、6 路 IP/账号/端口，以及 SNMP OID 槽位（按机型填值）。'));
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
