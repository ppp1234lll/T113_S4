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
    { id: 'camera', name: '摄像机' },
    { id: 'devices', name: '设备档案' },
    { id: 'switch', name: '交换机' },
    { id: 'media', name: '媒体' },
    { id: 'config', name: '配置' },
    { id: 'maint', name: '运维' }
  ];

  var st = { page: 'overview', ver: 0, version: '' };

  /* ==================== 登录 ==================== */
  function showLogin(msg) {
    $('#login').className = 'login';
    $('#app').className = 'app hidden';
    if (msg) $('#loginMsg').textContent = msg;
  }

  function showApp() {
    $('#login').className = 'login hidden';
    $('#app').className = 'app';
    buildNav();
    go(st.page);
  }

  function tryLogin(pass) {
    return api('/api/login', { method: 'POST', body: { pass: pass } }).then(function (r) {
      if (r.ok && r.d && r.d.token) { $('#loginMsg').textContent = ''; showApp(); return true; }
      showLogin(r.msg === 'bad password' ? '口令不正确' : (r.msg || '登录失败'));
      return false;
    }).catch(function () { showLogin('无法连接服务'); return false; });
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
    var c = $('#content');
    c.innerHTML = '';
    var builder = {
      overview: pageOverview, net: pageNet, camera: pageCamera, devices: pageDevices,
      switch: pageSwitch, media: pageMedia, config: pageConfig, maint: pageMaint
    }[page] || pageOverview;
    c.appendChild(builder());
  }

  /* ==================== 配置表单（通用） ==================== */

  /* 按点分键的前缀分组，让表单有层次（例：net.wan.fail_n → "net.wan" 组）。 */
  function groupOf(key) {
    var i = key.lastIndexOf('.');
    return (i < 0) ? key : key.slice(0, i);
  }

  function fieldFor(key, val) {
    var lab = el('label', 'field');
    var isBool = (typeof val === 'boolean');

    if (isBool) lab.className = 'field chk';
    lab.appendChild(el('span', null, key));
    lab.title = key;

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
      Promise.all(reqs).then(function (rs) {
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
          sec.appendChild(el('h3', null, g));
          var grid = el('div', 'grid');
          groups[g].forEach(function (k) { grid.appendChild(fieldFor(k, items[k])); });
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
    root.appendChild(configCard('网络参数（net.*）', ['net', 'sys'],
      '含内外网平台地址、无线/有线网口与传输模式。'));
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
    $('#loginForm').addEventListener('submit', function (ev) {
      ev.preventDefault();
      tryLogin($('#pass').value);
    });
    $('#logoutBtn').addEventListener('click', function () {
      api('/api/logout', { method: 'POST', body: {} }).then(function () {
        showLogin('已退出');
      });
    });

    /* 探测会话是否还在：随便取一个需要鉴权的接口，401 就回登录页。 */
    api('/api/overview').then(function (r) {
      if (r._status === 401) showLogin('');
      else if (r.ok) { st.version = (r.d && r.d.version) || ''; $('#verLabel').textContent = st.version; showApp(); }
      else showLogin('');
    }).catch(function () { showLogin('无法连接服务'); });
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', boot);
  else
    boot();
})();
