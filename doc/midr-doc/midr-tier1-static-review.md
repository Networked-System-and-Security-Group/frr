# Tier1 建邻准入静态逻辑复查

日期：2026-09-16。检查对象是当前工作区（包含未提交和新增文件），依据外层设计文档与 `midr-tier1-admission.md`。本次未提交代码。

## 结论

主要判定和接入流程符合最新约定：默认关闭；开启时明确命中 Tier1 拒绝；正常结束或执行超时且无命中证据放行；缺数据和执行设施错误等待/退避。发现并修复两项异步生命周期错误。静态检查通过，但未编译或执行 C/网络测试，不能据此宣称已经验证可运行。

## 已确认并修复

### 1. 新请求和被动握手可能触发许可提前失效

位置：`bgpd/bgp_midr_admission.c` 的 `ongoing_permit()`、`start_trace()`、`tick()`。

原逻辑只在定时扫描中保护配置 peer 的握手，未保护被动 doppelganger；`midr_admission_peer_ready()` 在许可观测超过 30 秒时还可直接启动新探测。`start_trace()` 清零条目许可，使已经绑定旧许可的合法握手在 Established 前检查失败。这与“新鲜度只限制新尝试”不符。

修复：统一检查配置连接及被动分身的有效在途许可；启动探测前也执行此保护。新尝试仍不能使用超龄许可，已有握手结束后才允许重新探测。策略、数据、源或 owner 失效仍正常撤销许可。

### 2. 取消锚点评估未撤销等待准入的锚点

位置：`bgpd/bgp_midr_nds.c` 的 `midr_nds_anchor_ctx_clear()`。

ANCHOR 决策提交异步准入后立即清空 `anchor_group_id[]`。随后手动换组、取消或重开 join 时，原清理函数因槽位和探测定时器均为空直接返回；准入条目中的 CL_ANCHOR owner 仍存在，旧结果可能建立已放弃轮次的边。

修复：在空上下文提前返回之前撤销 CL_ANCHOR owner。无其它 owner 时取消请求并释放条目；MANUAL 等其它需求继续保留。取消后的迟到回调沿用已有 token 失效机制，不恢复旧连接。

## 核查范围及保留边界

- 中央 control 入口在台账、adopt、PM、nudge 和 peer 创建之前执行准入；原生 peer 占址提前拒绝。
- BGP Start、主动 socket、被动 accept 及 Established 前均有所有权限定的 guard；取消 token 先清 owner，再取消订阅。
- 源地址及实例/VRF 参与调度键；UDP 显式绑定源地址，失败不回落；非默认 VRF 明确不支持。
- 两份数据成功发布/清除通知准入失效；正常缺 hop/映射不被当作 Tier1 命中。
- 意图容量 128、控制宽限 120 秒、已有 Established 会话保留等是实现文档明示的边界，没有擅自改变。
- 另有 `midr peer session ...` 调试/辅助命令，经 `bgp_midr.c:midr_peer_session_request()` 直接配置普通 BGP peer，不设置 `PEER_FLAG_MIDR_OVERLAY`，因此不受此策略约束。这与当前按所有权限定的 guard 一致，但不能把它等同于受准入保护的 `midr session ...`；如要统一两种命令，需要另外确定普通 peer 的所有权和兼容语义，本次未直接改写该接口。

## 验证

本机执行 `python tools/check_midr_native_static.py` 和 `git diff --check`，均通过。结构脚本不验证 C 类型、链接、FSM 执行或真实 socket。

`test_midr_admission.c` 新增虚拟时间回归：主动/被动握手超过 30 秒仍保留原许可；握手消失后新尝试重新探测；锚点槽位已空时取消旧准入；多 owner 条目保留手工需求及取消后的迟到回调。测试链接参数增加 `--wrap=clock_gettime`，无需实际等待 31 秒。

这些 C 用例尚未执行。当前为 Windows 环境，WSL 枚举返回 `E_ACCESSDENIED`，未取得可用 Linux FRR 构建环境。后续需在 Linux 执行准入、engine、scheduler、UDP C 测试，并验证双端冷探测、主被动碰撞、配置重载和换组时的真实握手。
