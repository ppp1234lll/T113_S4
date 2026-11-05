@echo off
rem 运维箱通道配置 - 双击启动（无安装、无依赖）
rem 直接调用同目录的 tunnelcfg.ps1，绕过执行策略限制。
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0tunnelcfg.ps1"
