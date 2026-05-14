# RTSP Full Feature 落地计划

## 1. 目标

- 在 `server-lib` 中交付可商用 RTSP 服务端能力，覆盖拉流与推流主场景。
- 与现有架构保持一致：`protocol/<proto>` + `server/services` + `test/protocol/<proto>` + `docs/protocols`。
- 形成可持续迭代的协议实现、测试与运维基线。

## 2. 范围定义

### 2.1 In Scope

- RTSP 1.0 核心命令：
  - `OPTIONS`
  - `DESCRIBE`
  - `SETUP`
  - `PLAY`
  - `PAUSE`
  - `TEARDOWN`
  - `GET_PARAMETER`
  - `SET_PARAMETER`
  - `ANNOUNCE`
  - `RECORD`
- 传输：
  - RTP/RTCP over TCP（interleaved）
  - RTP/RTCP over UDP（unicast）
- 鉴权与安全：
  - Basic
  - Digest
  - ACL 与限流
- 媒体与协商：
  - SDP 解析/生成
  - 与 `RtcProto` / `RtcpProto` 对接
- 工程化：
  - 单测 + 集成测试 + 互通测试 + 压测
  - 指标、日志、文档、发布检查清单

### 2.2 Out of Scope（首版）

- RTSP 2.0
- 完整 ONVIF 功能集
- 大规模集群控制面与全局会话调度

## 3. 模块规划

```text
protocol/rtsp/
  include/
    rtsp.h
    rtsp_protocol.h
    rtsp_request.h
    rtsp_response.h
    rtsp_parser.h
    rtsp_session.h
    rtsp_transport.h
    rtsp_sdp.h
    rtsp_server.h
  src/
    rtsp_parser.cpp
    rtsp_session.cpp
    rtsp_transport.cpp
    rtsp_sdp.cpp
    rtsp_server.cpp

server/services/
  include/rtsp_service.h
  src/rtsp_service.cpp

test/protocol/rtsp/
  test_rtsp_parser.cpp
  test_rtsp_session.cpp
  test_rtsp_play.cpp
  test_rtsp_record.cpp
  test_rtsp_auth.cpp
  test_rtsp_interop.cpp
```

## 4. WBS（2 人并行，8 周基线）

### W1：架构与骨架

- 建立 `protocol/rtsp` 模块骨架与 `RtspProto` 构建接入。
- 落地 Request/Response/Parser/Session 基础对象。
- 建立 `RtspService` 生命周期托管骨架。
- 验收：项目可编译；`OPTIONS`/`DESCRIBE` 基础可通。

### W2：控制面状态机（拉流链路）

- 实现 `SETUP/PLAY/PAUSE/TEARDOWN/GET_PARAMETER/SET_PARAMETER` 状态机。
- 实现 CSeq、Session、Transport、超时与 KeepAlive 规则。
- 验收：VLC/FFmpeg 可稳定完成 TCP interleaved 拉流流程。

### W3：SDP 与媒体绑定

- 完成 SDP 解析/生成（H264/H265/AAC/OPUS 常见参数）。
- 对接 `RtcProto`/`RtcpProto`，打通 RTP/RTCP 数据路径。
- 验收：播放器可解码首帧，协商参数正确。

### W4：UDP 传输

- 实现 UDP unicast 下的 `SETUP`、端口分配与回包路径。
- 完成 TCP/UDP 双栈切换与冲突处理。
- 验收：同一流在 TCP/UDP 均可稳定播放。

### W5：推流能力（ANNOUNCE/RECORD）

- 实现推流会话状态机与上行 SDP 处理。
- 完成媒体接收、缓冲与会话隔离。
- 验收：FFmpeg/GStreamer 推流可持续接收。

### W6：安全与策略

- 实现 Basic + Digest 鉴权。
- 增加 ACL、请求限流、防暴力破解策略。
- 增加审计日志字段（client/session/stream/action/result）。
- 验收：鉴权与防护测试通过，异常流量可控。

### W7：稳定性与互通收敛

- 完成 24h+ soak 测试与并发压测。
- 完成 VLC/FFmpeg/GStreamer 与常见 IPC 互通修复。
- 验收：无崩溃、无明显泄漏，关键失败率达标。

### W8：发布准备

- 完成配置、运维、排障文档。
- 固化 CI 回归用例与发布检查清单。
- 验收：发布评审通过，可进入灰度。

## 5. 角色分工建议

- 工程师 A：协议控制面（Parser/State/Auth/Service）。
- 工程师 B：媒体数据面（RTP/RTCP/Transport/Test/Perf）。
- 每日集成，周级里程碑验收，避免末期大规模冲突。

## 6. 验收指标

- 功能：核心命令与异常路径覆盖完整。
- 互通：VLC / FFmpeg / GStreamer 通过；至少 2 类 IPC 实测通过。
- 稳定：24h 压测无 crash，资源增长可控。
- 性能：达到目标并发会话数（按部署级别设定 200/500/1000 档）。
- 安全：未授权请求拦截、Digest 回放防护、限流策略有效。

## 7. 主要风险与应对

- 客户端差异导致状态机分支复杂：
  - 应对：优先做协议兼容测试矩阵，持续回归。
- UDP/NAT 场景边界多：
  - 应对：端口生命周期与超时回收做专项测试。
- Digest 实现细节多：
  - 应对：先通过 Basic 打通，再分阶段接入 Digest。

## 8. 里程碑出口标准

- M1（W2 末）：可稳定拉流（TCP）。
- M2（W4 末）：TCP + UDP 双传输可用。
- M3（W5 末）：推拉流双能力可用。
- M4（W8 末）：满足发布与灰度条件。

## 9. 当前实现快照（持续更新）

- 本轮冲刺范围（冻结）：
  - `SETUP` Transport 多候选协商（逗号分隔）与首个可用项选择。
  - Session 错误语义细化：缺失/未知/过期区分。
  - `PLAY Range` 互通增强（保留 `npt=now-` 规范化输出）。
  - 最小媒体面桥接（RTSP 会话侧占位打通 `RtcProto/RtcpProto`）。
  - RTSP over TCP interleaved（`$` 帧）接收与混流解析。
  - 连接拆包器可测试化（文本 RTSP 与 `$` 帧统一状态机）。
  - 安全策略补齐一期：Digest 鉴权最小闭环、ACL、请求限流与错误凭据惩罚。
  - RTCP 桥接增强：SR 活动透传到会话，RR 输出 `last_sr/dlsr`，会话级 SR/RR 统计可观测。
  - 真实媒体面链路推进：新增 interleaved RTP/RTCP 出站帧构建接口（按会话+track 映射 channel）。
  - 互通矩阵一期：新增 FFmpeg/VLC/GStreamer 典型流程回归用例（拉流/推流/鉴权混合场景）。
  - 协议边界回归扩展：多 track 复用、重连重建、短超时过期、CSeq 乱序与传输混用路径。
  - 可观测性一期：请求/状态码指标快照、审计事件环形缓存、可选结构化日志输出。
- 本轮验收标准：
  - 多候选协商路径具备成功与失败回退用例。
  - 过期会话返回 `408` 且携带 `Session` 头，未知会话返回 `454`。
  - `npt=now-` 在可播放状态下被接受并回显为 `npt=now-`。
  - RTSP 测试集 `ctest -R rtsp` 全绿。
  - 文本 RTSP 与 interleaved `$` 帧可在同连接内被分帧处理。
  - 覆盖半包/粘包分帧回归（header/body/interleaved）。
  - Digest challenge/response 成功与失败路径具备回归。
  - ACL 拒绝路径返回 `403`；限流命中返回 `429`。
  - SR 输入后 RR 输出携带非零 `last_sr`，并可通过会话快照读取 SR/RR 计数。
  - 可按会话与 track 生成 `$` 帧（RTP、RR、SR），用于后续连接层实际回写。
  - 互通回归 `rtsp_interop` 全绿并并入 `ctest -R rtsp`。
  - 边界回归覆盖：`test_rtsp_session/test_rtsp_server/test_rtsp_state_matrix` 新增 edge 场景并全部通过。
  - 可观测性回归覆盖：指标计数与审计事件查询路径已纳入 `test_rtsp_server`。

- 已完成模块与构建接入：`protocol/rtsp`、`RtspProto`、`test/protocol/rtsp`。
- 已完成基础控制面：`OPTIONS/DESCRIBE/SETUP/PLAY/PAUSE/TEARDOWN/GET_PARAMETER/SET_PARAMETER/ANNOUNCE/RECORD`。
- 已完成核心规则：
  - Session 成员级存储（跨请求复用）+ 超时回收。
  - CSeq 递增校验。
  - TCP interleaved 信道冲突检测。
  - UDP `client_port` 校验（RTP 偶数、RTCP=RTP+1）。
  - `ANNOUNCE` 需有效 SDP，且 codec 在 `H264/H265/AAC/OPUS` 白名单。
  - `RECORD` 需先 `ANNOUNCE`。
  - `ANNOUNCE` 与 `SETUP track` 数量一致性校验（不一致返回 `455`）。
  - 重复 `ANNOUNCE` 允许同构幂等提交（媒体指纹一致），不一致返回 `455`。
  - `PLAY` 支持 `Range/Scale`，并做参数校验与规范化输出（`Range` 固定三位小数）。
  - `SETUP` 支持多候选 `Transport` 协商（自动选择第一个受支持且参数有效的候选项）。
  - Session 错误语义细化：缺失/未知会话 `454`，过期会话 `408`。
  - 已支持 Basic 鉴权最小闭环：未授权返回 `401 + WWW-Authenticate`，授权后通过。
  - 已支持 Digest 鉴权最小闭环：下发 `nonce` challenge，校验 `response`（`MD5/qop=auth`）。
  - 支持 Basic + Digest 并行 challenge 下发与鉴权回退。
  - 鉴权互通增强：`OPTIONS` 保持免鉴权；`authorization` 小写头可被正确识别。
  - 新增 ACL 策略：支持按 `IP/CIDR/URI 前缀` 允许或拒绝（默认策略可配置）。
  - 新增请求限流与鉴权失败惩罚：滑窗请求上限、错误凭据计数与封禁时窗。
  - 新增安全统计快照：`acl_denied/rate_limited/auth_banned/auth_basic_fail/auth_digest_fail/auth_success`。
  - 增强媒体桥接观测：支持会话级 `build_sender_report` 与 `media_bridge_snapshot`。
  - RTCP 会话增强：暴露 `stats_snapshot`，累计 `rr/sr` 构建计数，并在 RR 报告块填充 `last_sr/dlsr`。
  - 新增出站帧构建：`build_interleaved_rtp_frame/build_interleaved_receiver_report_frame/build_interleaved_sender_report_frame`。
  - 新增互通测试集：`test_rtsp_interop`，覆盖 FFmpeg TCP 拉流、VLC UDP 拉流、GStreamer 双 track 推流、Basic+Digest challenge 兼容。
  - 增强边界测试：
    - `RtspSession` track channel 映射更新与 UDP 非 interleaved 负路径。
    - `RtspServer` 同 session 多 track + mixed TCP/UDP + keepalive 超时 + reconnect。
    - 状态矩阵覆盖 setup 路径 CSeq 乱序与过期后重建语义。
  - 新增可观测接口：
    - `metrics_snapshot()`：请求总数、方法计数、状态码计数与 2xx/4xx/5xx 汇总。
    - `recent_audit_events(max)`：最近审计事件（client/session/method/status/action/result）。
    - `configure_observability(...)`：日志开关、审计开关与审计容量配置。
  - 引入最小媒体面占位桥接：会话级 `RtpSessionManager/RtcpSession` 生命周期随 RTSP Session 管理。
  - 增加会话媒体入口接口：可注入 RTP 并导出 RTCP RR（用于后续真实数据面接线）。
  - 连接读帧升级：按 `Content-Length` 组完整请求帧并支持单连接流水请求解析。
  - 已接入 interleaved `$` 帧路径：按 channel 映射会话，RTP 入 `RtpSessionManager`，RTCP SR 更新 `RtcpSession` 发送者活动。
  - 会话级 interleaved 映射细化：支持 track 级 channel 绑定解析、冲突判定覆盖多 track。
  - 新增 interleaved 处理结果语义（RTP/RTCP handled、unknown channel、malformed RTP/RTCP），便于后续日志与指标接入。
  - 细分 interleaved 过期语义：当交错帧触发会话过期回收时返回 `session_expired` 结果。
  - 鉴权行为补充回归：`SETUP` 无鉴权返回 `401` 且含 challenge，`OPTIONS` 在鉴权开启下仍放行。
  - 新增 `RtspStreamFramer` 与测试集 `test_rtsp_framing`：覆盖 RTSP 文本帧与 `$` 帧的半包/粘包拆包行为。
  - 增加 interleaved 统计快照接口：可读取 handled/unknown/malformed/expired 计数。
  - 扩展 Range/Scale 互通回归：最小/最大 Scale、高精度 Scale 归一化、非数值 Scale 与更多 CSeq 边界。
- 当前测试覆盖：`test_rtsp`、`test_rtsp_session`、`test_rtsp_play`、`test_rtsp_server`、`test_rtsp_state_matrix`。
- 尚未完成：连接层自动回写调度（按 PLAY/RECORD 生命周期推送 RR/SR/RTP）、UDP 路径完整回发、稳定性/压测与 CI 门禁固化、Digest 高级能力（nonce 复用防重放、stale/opaque、算法扩展）。

- 本轮新增高优先收敛：
  - 自动回写调度：在 `PLAY/RECORD` 以及 interleaved 媒体输入后自动排队 RTCP 反馈（RR/SR），并提供 `drain_outbound_packets()` 统一消费出口。
  - UDP 回发闭环：对 UDP 轨道按 `client_port` 计算并排队 RTCP 回发目标端口（默认 RTP+1）。
  - Digest 增强：支持 `opaque` 校验、`stale=TRUE` challenge、`nonce+username+cnonce` 维度 `nc` 防重放单调校验，以及 `MD5/SHA-256` challenge 下发与算法解析。
  - 新增预检脚本：`release/rtsp/scripts/rtsp_preflight.ps1`（构建 RTSP/RTCP 目标并执行 gate）。
  - 连接层实发接线：`handle_connection` 已按 `connection_id` 绑定 session owner，并在 RTSP 响应后/交错帧处理后尝试写回 interleaved 自动反馈包。
  - UDP 自动反馈实发：出站 `udp_unicast` 包在连接层使用客户端源 IP + `client_rtcp_port` 直接 `sendto` 回发；连接关闭后仍可独立发送 UDP 反馈。
  - 可观测性补强：新增 outbound 指标（`outbound_interleaved_sent` / `outbound_udp_sent` / `outbound_udp_failed`）以及 `flush_udp_outbound_packets(...)` 手动冲刷能力，便于运维排障与测试注入。
  - UDP 失败观测增强：新增 `outbound_udp_failed_by_track` 分桶统计，以及 OUTBOUND 审计事件 `detail` 字段（记录失败 track/port）。
  - UDP 重试策略一期：失败后按指数退避（25ms 起步，封顶 1s）回队，最多重试 2 次；超限后计入 dropped。新增指标 `outbound_udp_retried/outbound_udp_dropped` 与审计动作 `udp_retry/udp_drop`。
  - UDP 重试参数化：通过 `RtspObservabilityConfig` 可配置 `udp_retry_max_retries/udp_retry_base_backoff_ms/udp_retry_max_backoff_ms`，并已补 `max_retries=0` 直 drop 回归。
  - 工程化文档补齐：新增 `RTSP_CI_INTEGRATION.md`（Jenkins/GitLab/GitHub 示例）与 `RTSP_SOAK_REPORT_TEMPLATE.md`（soak 归档模板）。
  - 发布收口补齐：新增 `RTSP_RELEASE_CHECKLIST.md`（一页式放行清单）与 `RTSP_UDP_NAT_VALIDATION.md`（UDP/NAT 验证模板），并补并发验收脚本 `run_rtsp_concurrency.ps1/.sh` 与 GitHub Actions 工作流 `.github/workflows/rtsp-gate.yml`。

## 10. 稳定性与门禁执行说明（新增）

- 本地/CI 统一门禁脚本（Windows PowerShell）：
  - `release/rtsp/scripts/run_rtsp_gate.ps1`
  - 用法：`pwsh -File release/rtsp/scripts/run_rtsp_gate.ps1 -BuildDir build-mingw -Regex 'rtsp|rtcp'`
- 本地/CI 统一门禁脚本（Linux/macOS Bash）：
  - `release/rtsp/scripts/run_rtsp_gate.sh`
  - 用法：`bash release/rtsp/scripts/run_rtsp_gate.sh build rtsp\|rtcp`
- 本地 soak 脚本（迭代执行并生成报告）：
  - `release/rtsp/scripts/run_rtsp_soak.ps1`
  - 用法：`pwsh -File release/rtsp/scripts/run_rtsp_soak.ps1 -BuildDir build-mingw -DurationSec 3600 -Parallel 4 -Regex 'rtsp' -OutDir .\\logs\\rtsp_soak`
  - 输出：
    - `driver-<timestamp>.log`：每轮 ctest 原始输出与批次日志。
    - `iterations-<timestamp>.jsonl`：逐轮耗时/退出码记录。
    - `summary-<timestamp>.json`：总轮次、失败数、平均耗时等摘要。
- 本地 soak 脚本（Linux/macOS Bash）：
  - `release/rtsp/scripts/run_rtsp_soak.sh`
  - 用法：`bash release/rtsp/scripts/run_rtsp_soak.sh build 3600 4 1 rtsp ./logs/rtsp_soak`
- 预检脚本：
  - Windows：`release/rtsp/scripts/rtsp_preflight.ps1`
  - Linux/macOS：`release/rtsp/scripts/rtsp_preflight.sh`
  - 统一索引：`release/rtsp/scripts/README.md`
- 建议发布前基线：
  - 先执行 gate（`rtsp|rtcp` 必须全绿）。
  - 再执行 24h soak（`DurationSec=86400`，失败阈值按环境设定）。
  - 归档日志与 JSON 摘要到发布工单。
