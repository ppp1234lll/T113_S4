============================================================
src/modules/gps  ——  GPS / NMEA 定位与时间源
============================================================

【目录作用】
  把串口上来的 NMEA 0183 文本流解析成两方面东西：定位（经纬度/海拔/速度/航向/
  质量/卫星数/HDOP）与 UTC 时间（供无 RTC 的板子校时）；并收拢"该不该上报"的
  判断。属模块层（`libivmodules.a`），依赖 `libivcore`（iv_log / iv_ret）与
  vendored 的 `third_party/minmea`。**本模块不打开、不读串口**——字节由调用方
  （通常是 `iv_serial_open()` 的 fd 加进 Reactor 后）喂进来。

【文件说明】
  iv_gps.c      解析 / 合并定位与时间、上报判定、安全校时（唯一使用 minmea 的文件）
  .gitkeep      目录占位

【实现状态】
  已实现（功能开发计划 M2-S2.5）。
  板端验证方式＝**灌样本**：UART1 设备树当前 `status = "disabled"`，无
  `/dev/ttySAC1` 节点，真实硬件联调要等设备树启用后（部署侧前置条件，非模块缺陷）。

【约定与注意事项】
  - minmea 只在本 `.c` 内出现，`iv_gps.h` 不暴露其类型；第三方源码一字未改，
    单独用一套编译标志（不施加本工程 `-Werror`）。
  - 未知量一律填 `IV_GPS_NA`（= NAN）而非 0——避免把"未定位"误当"在赤道"。
  - **无效定位不采信坐标**：只认 GGA `fix_quality > 0` 或 RMC `valid == 'A'`，
    见无效标志立即把 `fix.valid` 清 0。
  - **日期与时刻分开到达必须合并**，日期来源（RMC/ZDA）到过之后 `have_time`
    才成立，是校时的前提。
  - 校时策略"宁可不动不可乱动"：日期早于 `min_year`（默认 2024）拒绝；
    偏差分级（`adjtime` 渐进 / `settimeofday` 跳变 / 超 `big_adj_ms` 只审计）；
    两次校时间隔不小于 `sync_interval_ms`；`timeops` 可注入供单测，单测不真改时钟。
  - **不新增 `gps.*` 配置分类**（架构 §10.2 的 8 个一级分类是冻结口径），
    阈值走 `iv_gps_cfg_t` 编译期默认值＋调用方覆盖。
  - 硬件事实（2026-09-30）：GPS 接 UART1，引脚 PD21/PD22，**9600 8N1 无流控**；
    装配层打开该链路须用 `IV_GPS_UART_BAUD`，**不要**用 `iv_serial_cfg_default()`
    的 115200（那是采集板链路，波特率错配表现为收不到数据，易误判为接线问题）。
  - 返回码（iv_ret.h）：`IV_OK` / `IV_EINVAL` / `IV_EAGAIN`（无值得上报的变化或
    距上次校时太近）/ `IV_ERANGE`（偏差超限，只审计未调）/ `IV_ESTATE`（无可用日期）。

【相关文档】
  ivsbox/docs/功能开发计划.md：§S2.5
  T113运维终端-系统架构设计.md：§10.2
  ivsbox/third_party/minmea/README.ivsbox.md（第三方库本地登记）
