# tunnelcfg.ps1 - 运维箱通道配置（图形界面 + 命令行两用）
#
# 目的：把「设备 / 本机 exe / 服务器 frps」三处令牌与通道参数放在一个界面上，
#       每个动作都先说明"会改什么"，改完当场验证，不靠人去记三份配置文件。
#
# 用法：
#   双击 启动通道配置.cmd                  -> 图形界面
#   powershell -File tunnelcfg.ps1 -NoGui -Action check
#        动作：read / check / rotate / device-token / ini-token / dev-on / dev-off
#        退出码 0=全部成功，1=有失败（看 [X] 行）
#
# 本文件是 UTF-8 with BOM（PS 5.1 靠 BOM 才能正确显示中文）。

[CmdletBinding()]
param(
    [switch]$NoGui,
    [ValidateSet("read", "check", "rotate", "device-token", "ini-token", "dev-on", "dev-off", "ecs-token")]
    [string]$Action = "read",
    [string]$Token = "",
    [string]$DeviceIp = "192.168.1.64",
    [string]$DeviceUser = "admin",
    [string]$DevicePass = "",
    [string]$EcsHost = "47.95.22.96",
    [string]$EcsUser = "root",
    [string]$EcsPass = "",
    [string]$EcsKey = "",
    [string]$EcsHostKey = "ssh-ed25519 255 SHA256:Uy99qmPWoPgHefIFj1Am23sr0QVoC8RnpS43+ENUID4",
    [string]$IniPath = (Join-Path $env:LOCALAPPDATA "ivsbox-remote\remote.ini"),
    [int]$LocalSshPort = 16000
)

$ErrorActionPreference = "Stop"
$script:PLINK = "C:\Program Files\PuTTY\plink.exe"
$script:fail  = 0
$script:log   = New-Object System.Collections.ArrayList
$script:txt   = $null

# ------------------------------------------------------------------ 日志
function L {
    param([string]$m)
    [void]$script:log.Add($m)
    if ($script:txt) { $script:txt.AppendText($m + "`r`n"); $script:txt.SelectionStart = $script:txt.TextLength; $script:txt.ScrollToCaret() }
    else { Write-Host $m }
}
function Ok  { param([string]$m) L "[OK]   $m" }
function Bad { param([string]$m) L "[X]    $m"; $script:fail++ }
function Warn{ param([string]$m) L "[WARN] $m" }
function Step{ param([string]$m) L ""; L "== $m ==" }

# ------------------------------------------------------------------ 小工具
function New-Token32 {
    $b = New-Object byte[] 16
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $rng.GetBytes($b)
    -join ($b | ForEach-Object { $_.ToString("x2") })
}

function Test-TokenChars {
    param([string]$t)
    if (-not $t) { return $false }
    if ($t.Length -lt 1 -or $t.Length -gt 63) { return $false }
    return ($t -match '^[0-9A-Za-z\-_\.~/+@=]+$')
}

function Mask { param([string]$t) if (-not $t) { return "(空)" } if ($t.Length -le 6) { return $t } return ($t.Substring(0,4) + "…" + $t.Substring($t.Length-2) + " (len=$($t.Length))") }

function Get-IniLine {
    param([string]$path, [string]$key)
    if (-not (Test-Path -LiteralPath $path)) { return "" }
    $l = Get-Content -LiteralPath $path | Where-Object { $_ -match ("^" + [regex]::Escape($key) + "=") } | Select-Object -First 1
    if (-not $l) { return "" }
    return ($l -replace ("^" + [regex]::Escape($key) + "="), "")
}

function Set-IniKey {
    param([string]$path, [string]$key, [string]$val)
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $lines = Get-Content -LiteralPath $path
    $hit = $false
    $out = foreach ($l in $lines) {
        if ($l -match ("^" + [regex]::Escape($key) + "=")) { $hit = $true; "$key=$val" } else { $l }
    }
    if (-not $hit) { return $false }
    $out | Set-Content -LiteralPath $path -Encoding ASCII
    return $true
}

function Test-LocalPort {
    param([int]$port)
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect("127.0.0.1", $port, $null, $null)
        if ($iar.AsyncWaitHandle.WaitOne(800)) { $c.EndConnect($iar); return $true }
        return $false
    } catch { return $false } finally { $c.Close() }
}

# ------------------------------------------------------------------ 设备（HTTP）
function New-DeviceSession {
    param([string]$ip, [string]$user, [string]$pass)
    $s = New-Object Microsoft.PowerShell.Commands.WebRequestSession
    $body = @{ user = $user; pass = $pass } | ConvertTo-Json -Compress
    $r = Invoke-RestMethod -Uri "http://$ip/api/login" -Method Post -Body $body `
            -ContentType "application/json" -WebSession $s -TimeoutSec 12
    if (-not $r.ok) { throw "设备登录被拒（e=$($r.e) $($r.msg)）——口令不对，或该 IP 打不开" }
    return $s
}

function Get-DeviceStatus {
    param([string]$ip, $s)
    return (Invoke-RestMethod -Uri "http://$ip/api/tunnel" -WebSession $s -TimeoutSec 12)
}

function Set-DeviceToken {
    param([string]$ip, $s, [string]$tok, [int]$ttl = 0, [bool]$enable = $true)
    $body = @{ token = $tok; ttl = $ttl; enable = $enable } | ConvertTo-Json -Compress
    return (Invoke-RestMethod -Uri "http://$ip/api/tunnel/config" -Method Post -Body $body `
                -ContentType "application/json" -WebSession $s -TimeoutSec 30)
}

function Set-DeviceSwitch {
    param([string]$ip, $s, [bool]$on)
    $path = if ($on) { "/api/tunnel/enable" } else { "/api/tunnel/disable" }
    return (Invoke-RestMethod -Uri ("http://" + $ip + $path) -Method Post -Body "{}" `
                -ContentType "application/json" -WebSession $s -TimeoutSec 20)
}

# ------------------------------------------------------------------ 服务器（plink）
function Get-EcsRemoteScript {
    param([string]$tok)
    return @'
set -e
TS=$(date +%Y%m%d-%H%M%S)
for f in /etc/frp/frps.toml /etc/frp/visitor.toml; do
  [ -f "$f" ] && cp -a "$f" "$f.bak-$TS"
done
sed -i 's|^auth\.token = ".*"|auth.token = "__TOKEN__"|' /etc/frp/frps.toml /etc/frp/visitor.toml
sed -i 's|^webServer\.password = ".*"|webServer.password = "__TOKEN__"|' /etc/frp/frps.toml
echo "--- after edit ---"
grep -h -E '^(auth\.token|webServer\.password)' /etc/frp/frps.toml /etc/frp/visitor.toml
systemctl restart frps
systemctl restart frpv 2>/dev/null || true
sleep 2
echo "--- services ---"
systemctl is-active frps
systemctl is-active frpv 2>/dev/null || true
ss -lntp 2>/dev/null | grep -E ':(7000)[[:space:]]' || echo "WARN: 7000 not listening"
'@.Replace("__TOKEN__", $tok)
}

function Invoke-EcsToken {
    param([string]$tok, [string]$pass, [string]$key)
    if (-not (Test-Path $script:PLINK)) { throw "没找到 plink.exe（$($script:PLINK)），无法自动改服务器；请用「复制服务器命令」手工执行" }
    $a = @("-ssh", "-batch")
    if ($key) { $a += @("-i", $key) } else { $a += @("-pw", $pass) }
    if ($EcsHostKey) { $a += @("-hostkey", $EcsHostKey) }
    $a += @("$EcsUser@$EcsHost", (Get-EcsRemoteScript $tok))
    $out = & $script:PLINK @a 2>&1
    $rc = $LASTEXITCODE
    foreach ($line in $out) { L ("    " + $line) }
    if ($rc -ne 0) { throw "服务器侧命令失败（rc=$rc）——令牌没改成，服务未重启" }
    if (($out -join "`n") -match "WARN: 7000 not listening") { throw "frps 重启后 7000 没监听" }
    return $true
}

# ------------------------------------------------------------------ 动作
function Do-Read {
    Step "读取现状"
    $it = Get-IniLine $IniPath "token"
    $ih = Get-IniLine $IniPath "host"
    $ip = Get-IniLine $IniPath "port"
    if (Test-Path -LiteralPath $IniPath) {
        Ok "本机 exe 配置：$IniPath"
        L  "       服务器 $ih`:$ip   令牌 $(Mask $it)"
    } else {
        Warn "本机 exe 配置不存在：$IniPath（exe 首次运行时才生成）"
    }
    $lp = if (Test-LocalPort $LocalSshPort) { "在监听（通道已通）" } else { "没监听（通道未通）" }
    L  "       本机 ssh 端口 $LocalSshPort ：$lp"

    try {
        $s = New-DeviceSession $DeviceIp $DeviceUser $DevicePass
        $d = Get-DeviceStatus $DeviceIp $s
        Ok "设备 $DeviceIp 可达"
        L  "       SN=$($d.d.sn)  通道=$(if($d.d.on){'已开启'}else{'未开启'})  令牌长度=$($d.d.token_len)"
        L  "       frps=$($d.d.server):$($d.d.server_port)  配置=$($d.d.conf)"
        L  "       累计开启=$($d.d.stat.opens)  拒绝=$($d.d.stat.denies)"
        return $d
    } catch {
        Bad "设备 $DeviceIp 读不到：$($_.Exception.Message)"
        return $null
    }
}

function Do-Check {
    Step "三处一致性检查"
    $it = Get-IniLine $IniPath "token"
    $ih = Get-IniLine $IniPath "host"
    $d = $null
    try {
        $s = New-DeviceSession $DeviceIp $DeviceUser $DevicePass
        $d = Get-DeviceStatus $DeviceIp $s
    } catch { Bad "设备 $DeviceIp 读不到：$($_.Exception.Message)" }

    if ($it) { Ok "本机令牌：$(Mask $it)" } else { Warn "本机令牌：空（exe 会用内置默认值）" }
    if ($d) {
        if ($d.d.token_set) { Ok "设备令牌：已设置，长度 $($d.d.token_len)" } else { Warn "设备令牌：未设置" }
        if ($d.d.token_len -eq $it.Length) { Ok "本机与设备令牌长度一致（$($it.Length)）" }
        else { Bad "本机与设备令牌长度不一致：本机 $($it.Length) / 设备 $($d.d.token_len) ⇒ 两者不是同一个值" }
        if ($ih -and $d.d.server -and $ih -ne $d.d.server) { Bad "本机服务器 $ih 与设备 $($d.d.server) 不是同一台" }
        elseif ($ih) { Ok "服务器地址一致：$ih`:$($d.d.server_port)" }
        if (-not $d.d.on) { Warn "设备通道处于关闭状态（enabled=$($d.d.enabled)）" }
    }
    if (Test-LocalPort $LocalSshPort) { Ok "通道实测：本机 $LocalSshPort 在监听（frpc 已登录成功）" }
    else { Warn "通道实测：本机 $LocalSshPort 没监听 —— 要么 exe 没开，要么 frpc 登录失败（看 exe 日志）" }
    L ""
    L "结论：设备与服务器只能证明'长度/地址'，'值是否相同'要看 frpc 是否登录成功（上面那条实测）。"
}

function Do-DeviceToken {
    param([string]$tok)
    if (-not (Test-TokenChars $tok)) { Bad "令牌含非法字符或长度越界（1~63，允许 0-9 A-Z a-z - _ . ~ + / = @）"; return }
    Step "把令牌下发到设备（$DeviceIp）"
    $s = New-DeviceSession $DeviceIp $DeviceUser $DevicePass
    Ok "设备登录成功"
    $r = Set-DeviceToken $DeviceIp $s $tok 0 $true
    if (-not $r.ok) { Bad "下发失败：e=$($r.e) $($r.msg)"; return }
    Ok "设备已接受：令牌长度 $($r.d.token_len)，通道 on=$($r.d.on)"
    L  "       设备立刻重写 secure/tunnel.token 与 frpc.toml，monitor.sh 拉起 frpc 重连；secretKey 自动重算"
    Start-Sleep -Seconds 6
    $d = Get-DeviceStatus $DeviceIp $s
    if ($d.ok -and $d.d.token_set) { Ok "复查：token_set=true，通道 $(if($d.d.on){'已开启'}else{'未开启'})" }
    else { Bad "复查失败：设备没报 token_set" }
}

function Do-IniToken {
    param([string]$tok)
    if (-not (Test-TokenChars $tok)) { Bad "令牌含非法字符或长度越界"; return }
    Step "写入本机 exe 配置"
    if (-not (Test-Path -LiteralPath $IniPath)) {
        Bad "配置文件不存在：$IniPath（先在设备上跑一次 exe，让它生成）"
        return
    }
    if (Set-IniKey $IniPath "token" $tok) { Ok "已写入 token=（长度 $($tok.Length)）" }
    else { Bad "这个 ini 里没有 token= 这一行，请手工检查：$IniPath" }
    L  "       生效方式：关掉「运维箱远程访问.exe」再打开（程序启动时读 ini）"
}

function Do-Rotate {
    Step "一键轮换令牌（顺序：服务器 → 设备 → 本机，任一步失败即停）"
    if (-not $Token) { $Token = New-Token32; L "    生成新令牌：$Token" }
    if (-not (Test-TokenChars $Token)) { Bad "令牌非法"; return }

    L ""
    L "将依次改动："
    L "  1) $EcsHost`: /etc/frp/frps.toml(auth.token/webServer.password) + visitor.toml(auth.token)，重启 frps/frpv"
    L "  2) 设备 $DeviceIp`：secure/tunnel.token + frpc.toml，frpc 重连"
    L "  3) 本机 $IniPath`：token="
    L "影响：该 frps 上【所有】设备都会在 1) 之后断线，直到各自执行 2)。"
    L ""

    try {
        Step "1/3 服务器"
        Invoke-EcsToken $Token $EcsPass $EcsKey | Out-Null
        Ok "服务器已切换并重启，7000 在监听"
    } catch { Bad "服务器步骤失败：$($_.Exception.Message)"; L "        已中止：设备和本机都没动，三处仍是旧值，服务可用"; return }

    try {
        Do-DeviceToken $Token
        if ($script:fail -gt 0) { L "        设备步骤已失败：本机配置先不改，避免出现'两边不同值'"; return }
    } catch { Bad "设备步骤失败：$($_.Exception.Message)"; return }

    try {
        Do-IniToken $Token
        if ($script:fail -gt 0) { return }
    } catch { Bad "本机步骤失败：$($_.Exception.Message)"; return }

    Step "收尾"
    Ok "三处已同值。现在：关掉并重开「运维箱远程访问.exe」，再点刷新。"
    L  "       别的设备（同一台 frps 上的其他 SN）也要各自执行一次'下发令牌到设备'。"
}

function Do-Switch {
    param([bool]$on)
    Step ("把设备通道" + $(if ($on) { "打开" } else { "关闭" }))
    $s = New-DeviceSession $DeviceIp $DeviceUser $DevicePass
    $r = Set-DeviceSwitch $DeviceIp $s $on
    if ($r.ok) { Ok ("已" + $(if ($on) { "开启" } else { "关闭" }) + "：on=$($r.d.on)") }
    else { Bad "失败：e=$($r.e) $($r.msg)" }
}

function Do-EcsToken {
    param([string]$tok)
    if (-not (Test-TokenChars $tok)) { Bad "令牌非法"; return }
    Step "只改服务器（$EcsHost）"
    try { Invoke-EcsToken $tok $EcsPass $EcsKey | Out-Null; Ok "服务器已切换并重启" }
    catch { Bad $_.Exception.Message }
}

# ------------------------------------------------------------------ 命令行入口
function Invoke-Action {
    switch ($Action) {
        "read"         { Do-Read | Out-Null }
        "check"        { Do-Check }
        "device-token" { Do-DeviceToken $Token }
        "ini-token"    { Do-IniToken $Token }
        "ecs-token"    { Do-EcsToken $Token }
        "dev-on"       { Do-Switch $true }
        "dev-off"      { Do-Switch $false }
        "rotate"       { Do-Rotate }
    }
}

if ($NoGui) {
    if (-not $DevicePass) { $DevicePass = $env:IVSBOX_WEB_PASS }
    try { Invoke-Action } catch { Bad "未捕获错误：$($_.Exception.Message)" }
    if ($script:fail -eq 0) { Write-Host ""; Write-Host "RESULT: 全部成功"; exit 0 }
    Write-Host ""; Write-Host "RESULT: $($script:fail) 项失败"; exit 1
}

# ------------------------------------------------------------------ 图形界面
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$F = New-Object System.Windows.Forms.Form
$F.Text = "运维箱通道配置 1.0  ——  设备 / 本机 / 服务器 三处一处改"
$F.Size = New-Object System.Drawing.Size(760, 640)
$F.StartPosition = "CenterScreen"
$F.Font = New-Object System.Drawing.Font("Microsoft YaHei UI", 9)

function New-Label { param($text,$x,$y,$w=90)
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object System.Drawing.Point($x,$y)
    $l.Size = New-Object System.Drawing.Size($w,22); $l.TextAlign = "MiddleLeft"
    $F.Controls.Add($l); return $l
}
function New-Text { param($text,$x,$y,$w=180,$pw=$false)
    $t = New-Object System.Windows.Forms.TextBox
    $t.Text = $text; $t.Location = New-Object System.Drawing.Point($x,$y)
    $t.Size = New-Object System.Drawing.Size($w,22)
    if ($pw) { $t.UseSystemPasswordChar = $true }
    $F.Controls.Add($t); return $t
}
function New-Button { param($text,$x,$y,$w=120,$h=28)
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object System.Drawing.Point($x,$y)
    $b.Size = New-Object System.Drawing.Size($w,$h)
    $F.Controls.Add($b); return $b
}

# --- 区域标题
$g1 = New-Object System.Windows.Forms.GroupBox
$g1.Text = "① 服务器（frps）"; $g1.Location = New-Object System.Drawing.Point(10,8)
$g1.Size = New-Object System.Drawing.Size(724,96); $F.Controls.Add($g1)

New-Label "地址" 20 28
$tbHost = New-Text "" 70 28 150
New-Label "端口" 232 28
$tbPort = New-Text "7000" 272 28 60
New-Label "令牌" 344 28
$tbTok  = New-Text "" 384 28 200 $true
New-Label "SSH 口令" 20 58
$tbPw   = New-Text "" 100 58 120 $true
New-Label "（只用于 SSH 改 frps，不落盘）" 228 58 260

$btnGen   = New-Button "生成新令牌" 596 24 130 26
$btnCopy  = New-Button "复制服务器命令" 596 54 130 26

# --- 设备
$g2 = New-Object System.Windows.Forms.GroupBox
$g2.Text = "② 设备（ivsbox 箱体）"; $g2.Location = New-Object System.Drawing.Point(10,112)
$g2.Size = New-Object System.Drawing.Size(724,88); $F.Controls.Add($g2)

New-Label "设备 IP" 20 28
$tbIp   = New-Text "192.168.1.64" 90 28 130
New-Label "账号" 236 28
$tbUser = New-Text "admin" 276 28 80
New-Label "口令" 364 28
$tbDevPw= New-Text "" 404 28 110 $true
New-Label "（设备 Web 管理口令）" 522 28 190

# --- 本机
$g3 = New-Object System.Windows.Forms.GroupBox
$g3.Text = "③ 本机（运维箱远程访问.exe 配置）"; $g3.Location = New-Object System.Drawing.Point(10,208)
$g3.Size = New-Object System.Drawing.Size(724,72); $F.Controls.Add($g3)

New-Label "配置文件" 20 28
$tbIni = New-Text "" 90 28 590

# --- 操作
$g4 = New-Object System.Windows.Forms.GroupBox
$g4.Text = "④ 操作（都会先问一遍）"; $g4.Location = New-Object System.Drawing.Point(10,286)
$g4.Size = New-Object System.Drawing.Size(724,86); $F.Controls.Add($g4)

$btnCheck = New-Button "一致性检查" 20 24 130 30
$btnDev   = New-Button "下发令牌到设备" 158 24 140 30
$btnIni   = New-Button "写入本机配置" 306 24 130 30
$btnOn    = New-Button "开通道" 444 24 80 30
$btnOff   = New-Button "关通道" 532 24 80 30
$btnRot   = New-Button "一键轮换（三处同改）" 20 54 278 30
$btnSave  = New-Button "保存日志" 306 54 130 30
$btnClr   = New-Button "清空" 444 54 80 30

# --- 输出
$script:txt = New-Object System.Windows.Forms.TextBox
$script:txt.Multiline = $true
$script:txt.ScrollBars = "Vertical"
$script:txt.ReadOnly = $true
$script:txt.Location = New-Object System.Drawing.Point(10,382)
$script:txt.Size = New-Object System.Drawing.Size(724,210)
$script:txt.Font = New-Object System.Drawing.Font("Consolas", 9)
$script:txt.BackColor = [System.Drawing.Color]::FromArgb(250,250,250)
$F.Controls.Add($script:txt)

# --- 初始化：把 ini 灌进界面
$tbIni.Text = $IniPath
$tbHost.Text = Get-IniLine $IniPath "host"
$tbPort.Text = Get-IniLine $IniPath "port"
$tbTok.Text  = Get-IniLine $IniPath "token"
if (-not $tbHost.Text) { $tbHost.Text = $EcsHost }
if (-not $tbPort.Text) { $tbPort.Text = "7000" }

function Sync-Ui {
    $script:DeviceIp   = $tbIp.Text.Trim()
    $script:DeviceUser = $tbUser.Text.Trim()
    $script:DevicePass = $tbDevPw.Text
    $script:IniPath    = $tbIni.Text.Trim()
    $script:EcsPass    = $tbPw.Text
    $script:Token      = $tbTok.Text.Trim()
    $script:EcsHost    = $tbHost.Text.Trim()
}
function Ask { param($msg) return ([System.Windows.Forms.MessageBox]::Show($msg, "确认", "YesNo", "Question") -eq "Yes") }
function Run  {
    param([string]$what, [scriptblock]$sb)
    Sync-Ui
    $btnCheck.Enabled = $false; $btnDev.Enabled = $false; $btnIni.Enabled = $false
    $btnOn.Enabled = $false; $btnOff.Enabled = $false; $btnRot.Enabled = $false
    $before = $script:fail
    try { & $sb }
    catch { Bad "$what 出错：$($_.Exception.Message)" }
    finally {
        $btnCheck.Enabled = $true; $btnDev.Enabled = $true; $btnIni.Enabled = $true
        $btnOn.Enabled = $true; $btnOff.Enabled = $true; $btnRot.Enabled = $true
    }
    if ($script:fail -gt $before) { [System.Windows.Forms.MessageBox]::Show("有失败项，看下方输出。", "完成", "OK", "Warning") | Out-Null }
}

$btnGen.Add_Click({
    $t = New-Token32
    $tbTok.Text = $t
    $tbTok.UseSystemPasswordChar = $false
    L "已生成新令牌（明文显示，方便抄到别处）：$t"
})
$btnCopy.Add_Click({
    Sync-Ui
    $s = Get-EcsRemoteScript $script:Token
    [System.Windows.Forms.Clipboard]::SetText("bash -c '" + ($s -replace "'", "'\''") + "'")
    L "已复制到剪贴板：到 $($script:EcsHost) 上以 root 粘贴执行即可（无 plink 时用这条）"
})
$btnCheck.Add_Click({ Run "一致性检查" { Do-Check } })
$btnDev.Add_Click({
    Sync-Ui
    if (-not (Ask "把令牌下发给设备 $($script:DeviceIp)？`r`n`r`n设备会重写 secure/tunnel.token 与 frpc.toml，`r`nfrpc 立刻重连（几秒内通道会闪断一次）。")) { return }
    Run "下发令牌" { Do-DeviceToken $script:Token }
})
$btnIni.Add_Click({
    Sync-Ui
    if (-not (Ask "把令牌写进本机配置？`r`n`r`n$($script:IniPath)`r`n`r`n（要重开 exe 才生效）")) { return }
    Run "写入本机" { Do-IniToken $script:Token }
})
$btnOn.Add_Click({  Sync-Ui; Run "开通道" { Do-Switch $true } })
$btnOff.Add_Click({ Sync-Ui; Run "关通道" { Do-Switch $false } })
$btnRot.Add_Click({
    Sync-Ui
    if (-not $Token) { Warn "令牌为空，将自动生成" }
    if (-not (Ask "一键轮换会用同一串令牌依次改动：`r`n`r`n  1) 服务器 $($script:EcsHost)（需 SSH 口令，会重启 frps/frpv）`r`n  2) 设备 $($script:DeviceIp)`r`n  3) 本机 ini`r`n`r`n注意：该 frps 上【所有】设备会先断线，直到各自下发新令牌。`r`n`r`n继续？")) { return }
    Run "一键轮换" { Do-Rotate }
})
$btnSave.Add_Click({
    $d = New-Object System.Windows.Forms.SaveFileDialog
    $d.Filter = "文本 (*.txt)|*.txt"
    $d.FileName = "通道配置-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".txt"
    if ($d.ShowDialog() -eq "OK") {
        $script:log | Set-Content -LiteralPath $d.FileName -Encoding UTF8
        L "日志已保存：$($d.FileName)"
    }
})
$btnClr.Add_Click({ $script:txt.Clear() })

L "运维箱通道配置 1.0"
L "本机配置：$IniPath"
L "说明：任何一步改的都是【真实生效】的文件/接口，没有'预演'。要看不动手的结论，点「一致性检查」。"
L ""
[void]$F.ShowDialog()
