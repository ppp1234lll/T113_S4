# minmea（vendored 第三方库）

轻量级 GPS / NMEA 0183 解析库，**原样内嵌**在本工程中，供 M2-S2.5 的
`src/modules/gps/iv_gps.c` 使用。本文件是本工程的登记说明，不是上游文档；
上游 README 见同目录 `README.md`（未取，需要时按下面的来源自行拉取）。

## 一、来源与版本（可复核）

| 项 | 值 |
|---|---|
| 上游 | `https://github.com/kosma/minmea` |
| 固定 commit | `c43c9e7c5ed788122a9d8a5445679e1594f8a030`（`refs/heads/master`，2026-09-30 取） |
| 获取方式 | `https://codeload.github.com/kosma/minmea/tar.gz/refs/heads/master` |
| 获取日期 | 2026-09-30 |
| 本地文件 | `minmea.c`、`minmea.h`、`COPYING`、`LICENSE.MIT` —— **全部一字未改** |

### SHA256（复核"有没有被改动过"）

| 文件 | SHA256 |
|---|---|
| `minmea.c` | `3B30B322E513389B39A4E9610120A67CD8D1F8521BF541CFF724B8E21E20FBE1` |
| `minmea.h` | `D7F7817C4165D1867ADC84C3932C73019CE73E1B1A2F4A0EC42C2B8CFA9DFCE1` |
| `COPYING` | `EE820FF0DB4CE628569E0975AC27DC926052A9F85D102B101EDB104311EF4D90` |
| `LICENSE.MIT` | `92C301A7D025048AE6A2F36669C5BBCA5F448F65F20249F1E3CB88CDC3816B49` |

复核（任一 Linux 机器）：`sha256sum third_party/minmea/minmea.c third_party/minmea/minmea.h`

## 二、许可证（**需要人工拍板**）

`COPYING` 是 **WTFPL v2**（Do What The Fuck You Want To Public License, Version 2），
`minmea.h` 文件头亦如此声明；上游 `README.md` 的 Licensing 段写着
"see COPYING for amusement. Email me if the license bothers you and I'll happily
re-license under anything else under the sun."，仓库另附 `LICENSE.MIT` 与 `LICENSE.LGPL-3.0`。

- **对本工程的含义**：WTFPL 不要求署名、不要求衍生开源、**无任何条件**，闭源产品使用无障碍。
- **但若公司许可证白名单不接受 WTFPL**（它未经 OSI 认证，部分企业政策据此排除），
  有两条现成退路：① 依上游声明去函改为 **MIT**；② 换用其它 MIT/BSD 许可的 NMEA 实现。
- **这是合规判断，不由本模块自行决定**，已登记在修改记录与缺口清单中，等人工确认。

## 三、为什么不改上游文件

本工程编译标志含 `-Werror -Wall -Wextra`，上游代码未必满足（它自己的 CI 走
clang-analyzer ＋ libcheck 那套）。若为"编过"去改上游文件：① SHA256 不再可复核，
"这就是上游那一份"不再成立；② 上游更新时无法干净对比。
因此 `Makefile` 给 `third_party/%` **单独一条规则、编译时去掉 `-Werror`**
（保留 `-Wall -Wextra` 只为看得见警告）。**该纪律对后续所有 vendored 库同样适用。**

**推论：`git diff --check` 会在本目录上稳定报一条尾随空白**（`minmea.c` 第 80 行，上游自带）。
**按"原样不改"原则保留** —— 把"仓库空白整洁"排在"与上游字节一致、SHA256 可复核"之前是
捡芝麻丢西瓜。评审时看到这条告警属于**预期**，不是本工程引入的；若将来上游修掉它，
升级时这条告警会自然消失。

## 四、上游未被取用的部分（取舍说明）

| 上游文件 | 处理 |
|---|---|
| `tests.c`（官方测试，基于 libcheck） | **未纳入构建**（本工程单测自研，不引 libcheck）；但**其中的真实样本被摘入** `tests/unit/test_gps.c`（带正确校验和的 GGA/RMC/VTG/ZDA 与几类非法样本），借用了上游认证过的输入 |
| `example.c` | 未取（用法见上游 README，本工程按自己的接口封装） |
| `compat/`（Windows / TI-RTOS 兼容层） | 未取（目标平台是 Linux/glibc） |
| `.clusterfuzzlite/`、`CMakeLists.txt`、`.github/` | 未取（与本工程构建体系无关） |

## 五、本工程实际使用的上游 API（升级时对照此处）

`minmea_check`（严格模式）、`minmea_sentence_id`、`minmea_parse_gga`、`minmea_parse_rmc`、
`minmea_parse_vtg`、`minmea_parse_zda`、`minmea_tofloat`、`minmea_tocoord`、
`minmea_getdatetime`、`minmea_gettime`。

只要这些符号的语义不变，升级上游版本时 `iv_gps.c` 无需改动。
**已依赖的两条上游语义**（升级时重点核对）：
1. 空字段解析成 **`-1`**（不是 0）—— 本工程据此判断"日期/时刻缺失"；
2. 两位年由 `minmea_getdatetime` 按 `year < 80 ⇒ 20xx`、`year >= 1900 ⇒ 原样` 处理。
