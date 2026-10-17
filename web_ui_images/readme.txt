IVSBox Web 界面美化图片素材
============================

用途
  为 http://192.168.2.120/ 的 IVSBox Web 界面（ivsbox/web/static/）准备的
  配图素材，交由后续界面集成时使用。本目录只放图片，不放代码；
  集成方式见会话内预览页或下方"对应位置"。

对应位置（建议）
  brand/logo.svg        侧栏「IVSBox」文字左侧（约 22px）＋ 登录卡标题左侧（约 26px）
  brand/favicon.svg     浏览器标签页图标（index.html 目前没有 favicon，需补 <link rel="icon">）
  icons/*.svg           左侧导航 10 个页面图标（24×24 线性，stroke=currentColor，
                        用 <img> 引入后配 CSS 宽高 16px 即可跟随文字颜色）
  login-bg-dark.svg     登录页背景（深蓝科技风，推荐：给 .login 加 background 居中铺满）
  login-bg-light.svg    登录页背景（浅色版，与现有后台风格一致，二选一）
  empty-state.svg       列表/图片为空时的占位插画（摄像机、媒体、设备档案页可用）

来源与版权
  icons/     Lucide Icons v0.462.0（https://lucide.dev），ISC 许可证，可商用；
             原文件头部保留了许可证注释，请勿删除。
  其余       本项目自行绘制（SVG 源文件，无版权限制），配色取自现有主题
             （主色 #1f6feb，背景 #f4f6f8）。

备注
  - 图标在深色/选中态下的配色由引入方 CSS 控制，无需改 SVG。
  - 若需要 PNG 位图（如某些场景不支持 SVG），由使用方按需导出。
