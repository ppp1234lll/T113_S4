tests/web —— 前端（web/static）行为测试
========================================

为什么单独放一层
----------------
`web/static/app.js`、`index.html`、`style.css` 是**浏览器里**跑的代码：

- `make test`（C 单测）、`make itest`（要 ivsboxd 的集成测试）、`make arm`
  一个都碰不到它；
- 静态资源不从二进制里读、也不编译（见 web/src/iv_web_static.c 的说明），
  所以"编译通过"对前端不构成任何保证。

历史教训（D-16，2026-10-21）：提示条 `#toast` 原先挂在 `#content` 里面，
而 `go()` 每次切页都 `#content.innerHTML = ''` ⇒ 登录（showApp→go）那一刻就
被删掉，之后每次 `toast()` 都在 `$('#toast')` 上抛 TypeError。后果不是"少一个
提示"：`applyChanges` 里 toast 之后的 `st.ver = ...` 与 `after()` 全都不执行，
于是保存后版本号不更新、表单不重载，**下一次保存必然撞版本冲突**。
`node --check` 完全看不出来，C 侧所有用例也全绿。

用例清单
--------
ui_camera_slot.mjs
    摄像机通道建档：真实 index.html + app.js 在 jsdom 里跑起来，走
    登录 → 进「摄像机」页 → 选通道 → 填 IP → 点「建立该路通道」，
    断言 POST /api/config/apply 的 body（ver + 只发 1 个 camera.ch.N.ip 键）、
    空 IP 不发请求、建槽后下拉文案与 IP 回填。共 19 条断言。

ui_device_name.mjs
    总览页「设备名称」（`sys.device.name`）：断言输入框与「保存名称」按钮存在、
    未命名时只读行显示「未命名」（"空"与"没读到"要分得开）、保存只发 1 个键且
    版本号是**现取**的（本页没有 configCard 缓存 ver，拿旧值必撞 e=-3）。
    共 15 条断言。

ui_ops_log.mjs
    运维页「系统升级」+「系统日志」两张卡：/api/ota 字段渲染、启动计数 >1 带
    warn、日志列表文件名与下载链（`f` 做 URL 编码，日期里的 `/` 不编会被当路径段）、
    空列表与后端 `ok:false` 各有明确文案。共 16 条断言。

logs_e2e.py
    **不是 jsdom**，是真的进程：起一个 `build/host/ivsbox-web`（`--log-root` 指到
    /tmp 下造的假日志树），走真 HTTP 取证 `/api/logs` 与 `/api/logs/file`：
    形状白名单（非日期目录/非 .log 都不列）、mtime 降序、max 截断、正文与磁盘
    逐字节相同、Content-Disposition、`../` ⇒ 403、不存在 ⇒ 404、无 Cookie ⇒ 401、
    POST ⇒ 405、`/api/logsXYZ` 不归本层、日志根不存在 ⇒ 空列表。共 21 条断言。
    需要 python3（VM 上没有 node，所以这一半只能在 VM 上跑）。

怎么跑
------
jsdom **不是工程依赖**（不想把 node 生态塞进构建链），所以要单独准备：

    npm i jsdom                       # 装在任意目录，然后指 NODE_PATH
    NODE_PATH=<node_modules 路径> node tests/web/ui_camera_slot.mjs

    例：
    NODE_PATH=C:/Users/Think/.workbuddy/binaries/node/workspace/node_modules \
        node tests/web/ui_camera_slot.mjs

找不到 jsdom 时**打印 SKIP 并退出 0**（与 test_web_http 缺二进制时的口径一致），
不会把"没装环境"伪装成"跑过了且正常"。

端到端那条（在 ivsbox/ 目录下、先 `make` 出 build/host/ivsbox-web）：

    python3 tests/web/logs_e2e.py

退出码：0 通过（或 SKIP），1 有断言失败，2 用例自身崩了。

注意
----
`camera.ch.N.*` 在配置模块里是「按需变长数组」：出厂一条都没有，只有
`config.apply` 里出现 `camera.ch.N.<某键>` 时，`set_post_update()` 才抬高
`camera_count` 并整槽补齐 8 个键。这一点是**后端**行为，本目录的桩只是模拟它，
真实证据在 VM 上走 HTTP 端到端取的（见 docs/修改记录.md 2026-10-21 D-06）：
建槽前 18 项 / 0 条 camera.ch.* ⇒ 发 1 个 ip 键 ⇒ 26 项 / 8 条，重启后仍在。
