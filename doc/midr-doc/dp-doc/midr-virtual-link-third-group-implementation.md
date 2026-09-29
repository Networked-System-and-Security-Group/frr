# MIDR 虚链路：第三组实现任务说明

> 面向：第三组数据面实现
>
> 当前代码基线：分支 `feat/midr-three-way-integration`，HEAD
> `443c51474b827a8851aa8a0039c61b65e43db080`。
>
> 本文只定义第三组需要补齐的实现和接口边界，不决定第一组的邻接选择策略，也不替第一组调用 MIDR Session 或 Link 上报接口。

## 1. 实现目标

把当前“创建 GRE/ip6gre netdevice”的能力扩展为第一组可调用的完整虚链路设备服务：

```text
outer endpoint 可达
  -> 创建 GRE/ip6gre
  -> 配置 overlay 地址
  -> 设置接口 UP
  -> 确认 ifindex 和 overlay 可达性
  -> 返回 READY
  -> 支持删除、故障通知、重建和状态查询
```

第三组提供的是设备和数据面能力。第一组负责决定哪两个 MIDR 节点之间需要虚链路、何时调用本接口、如何建立 `midr_session_connect()` 以及何时调用 `midr_topology_link_upsert()`。

第三组不负责：

- 判断某条链路应当是物理链路还是虚链路；
- 选择 MIDR 对端或分配 MIDR `link_id`；
- 调用 `midr_session_connect()`；
- 调用 `midr_topology_link_upsert()` 或 `midr_topology_link_withdraw()`；
- 执行 SPF 或决定 MIDR 拓扑。

## 2. 当前已有实现

当前文件：

```text
midrd/midr-gre.h
midrd/midr-gre.c
lib/zclient.h
lib/zclient.c
zebra/zapi_msg.c
zebra/zebra_dplane.c
zebra/if_netlink.c
```

已有 API：

```c
midr_gre_interface_add()
midr_gre_interface_del()
midr_gre_interface_get_state()
midr_gre_interface_wait_up()
midr_gre_register_notify()
```

已有能力：

- 通过 `ZEBRA_GRE_ADD/DELETE` 创建或删除 GRE/ip6gre 设备；
- 支持外层 IPv4/IPv6 endpoint；
- 支持 `link_ifindex`、GRE key、封装标志和 MTU；
- 通过 Zebra interface 视图取得虚接口 ifindex；
- 支持 `PENDING/UP/FAILED/DOWN` 的基础注册表状态；
- 已有 `midrd/gre-link-tool.c` 和 `midr-test/midr-gre-connectivity-test.sh`。

当前 `struct midr_gre_tunnel` 主要描述外层隧道：

```text
ifname
vrf_id
outer local/remote
underlay link_ifindex
ikey/okey
encap_flags
mtu
```

其中：

```text
tun.link_ifindex = GRE 外层 underlay 出口接口
midr_gre_status.ifindex = 创建完成后的 GRE 虚接口 ifindex
```

两者不能混用。

## 3. 当前缺口

### 3.1 没有完整的虚链路描述

现有 API 没有描述隧道内部 overlay 地址，也不能表达完成虚链路所需的本端地址、对端地址和前缀长度。

需要增加一个高层虚链路描述，建议至少包含：

```text
设备名称（可选）
VRF
outer local/remote
outer underlay link_ifindex（可选）
overlay local address
overlay prefix length
overlay remote address（用于状态和可达性检查）
MTU
```

`link_id` 是否作为字段传入由第一组和第三组共同确定。第三组不得自行分配或改变第一组负责的逻辑 `link_id`。

### 3.2 没有配置 overlay 地址

当前 GRE API 创建设备后，没有通过生产接口执行：

```text
ip address add overlay-local/prefix dev greX
```

第三组需要通过 Zebra/ZAPI 或 FRR 统一的数据面接口完成地址配置，不应要求第一组调用 shell 或 raw netlink。

### 3.3 没有将接口置为 UP

当前 `midr_gre_status.state == UP` 主要表示 Zebra 已经发现设备并取得 ifindex，不表示：

```text
IFF_UP 已设置
overlay 地址已配置
overlay remote 已经可达
```

设备状态需要拆分为至少两个可观察阶段：

```text
DEVICE_UP       Zebra 已识别设备，ifindex 有效
READY            overlay 地址、接口状态和必要的可达性检查均完成
```

只有 `READY` 才能通知第一组继续进行虚链路建邻流程。

### 3.4 没有生产调用方和完整生命周期

当前 `midrd.c` 只初始化 GRE 模块，没有根据第一组请求创建虚链路的生产调用路径。需要补齐：

```text
第一组请求
  -> 第三组创建/配置设备
  -> READY/FAILED 通知
  -> 设备丢失通知
  -> 删除或重建
```

测试工具的手工 `ip addr add` 和 `ip link set up` 不能作为生产实现。

## 4. 建议的对外接口

可以扩展现有 `midr_gre_interface_*`，也可以新增更高层的 `midr_virtual_link_*`。建议新增高层接口，保留现有 GRE API 作为底层设备接口。

建议的概念接口如下，具体 C 签名由第一组和第三组联合确认：

```c
enum midr_virtual_link_state {
	MIDR_VLINK_DOWN = 0,
	MIDR_VLINK_CREATING,
	MIDR_VLINK_CONFIGURING,
	MIDR_VLINK_DEVICE_UP,
	MIDR_VLINK_READY,
	MIDR_VLINK_FAILED,
};

struct midr_virtual_link_desc {
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;

	/* GRE outer / underlay endpoints. */
	struct ipaddr outer_local;
	struct ipaddr outer_remote;
	ifindex_t outer_link_ifindex;

	/* GRE inner / overlay endpoint. */
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	uint8_t overlay_prefix_len;

	uint32_t mtu;
};

struct midr_virtual_link_status {
	enum midr_virtual_link_state state;
	char ifname[IFNAMSIZ];
	vrf_id_t vrf_id;
	ifindex_t ifindex;
	struct ipaddr overlay_local;
	struct ipaddr overlay_remote;
	int last_error;
};

int midr_virtual_link_add(struct midr_context *ctx,
			  const struct midr_virtual_link_desc *desc,
			  struct midr_virtual_link_status *status);
int midr_virtual_link_del(struct midr_context *ctx, const char *ifname,
			  struct midr_virtual_link_status *status);
int midr_virtual_link_get_state(struct midr_context *ctx, const char *ifname,
				struct midr_virtual_link_status *status);
int midr_virtual_link_wait_ready(struct midr_context *ctx, const char *ifname,
				 uint32_t timeout_ms,
				 struct midr_virtual_link_status *status);
```

状态通知应至少能够报告：

```text
DEVICE_UP
READY
FAILED
DOWN
```

如果不采用上述名称，也必须保留相同的语义区别。

## 5. 建立流程

### 5.1 创建

收到第一组请求后，第三组按以下顺序执行：

```text
1. 校验 outer local/remote 地址族；
2. 校验 overlay local/remote 地址族和 prefix length；
3. 确认 Zebra zclient 可用；
4. 通过现有 GRE ZAPI 创建设备；
5. 等待 Zebra interface add，取得 GRE ifindex；
6. 配置 overlay local address/prefix；
7. 设置接口 administratively UP；
8. 确认 overlay remote 可通过该设备到达；
9. 报告 READY、ifindex 和 overlay 地址。
```

如果 overlay 地址采用同一条点到点前缀，可以依靠接口地址产生 connected route；如果采用两个不在同一前缀的地址，则必须额外配置到 `overlay_remote` 的路由，或者在接口契约中明确不支持这种地址规划。

### 5.2 删除

删除流程需要：

```text
1. 停止新的配置和状态通知；
2. 删除 overlay 地址或将接口置 DOWN；
3. 通过 Zebra 删除 GRE 设备；
4. 清理本地 registry；
5. 报告 DOWN。
```

删除接口时不得调用第一组 Session 或 Link API；第一组负责先处理其逻辑邻接状态。

### 5.3 重连和设备丢失

如果 Zebra 重连、接口被外部删除或内核设备消失，第三组应：

```text
1. 报告 DEVICE_DOWN/FAILED；
2. 使旧 ifindex 失效；
3. 保留 descriptor，允许第一组决定是否重建；
4. 重建后返回新的 ifindex；
5. 不改变第一组负责的逻辑 link_id。
```

## 6. 错误处理和回滚

以下任一步骤失败时，不能报告 READY：

```text
GRE 创建
Zebra interface 确认
overlay 地址配置
接口置 UP
overlay endpoint 可达性检查
```

失败处理要求：

- 返回明确的 errno 风格错误；
- status 中保留失败阶段和 ifname/ifindex；
- 如果创建了设备但后续配置失败，回滚地址和设备，或明确报告可重试的 FAILED 状态；
- 重复请求相同描述必须幂等；
- 相同 ifname 但 endpoint 或 overlay 地址不同，必须拒绝或先执行明确的 rebind，不得静默覆盖；
- 不得把 `DEVICE_UP` 当成 `READY`。

## 7. 与第一组的接口边界

第一组负责：

```text
决定哪两个 MIDR 节点建立 overlay
提供稳定的 outer endpoint 和 overlay 地址规划
调用虚链路 API
根据 READY 状态调用 midr_session_connect()
等待 HELLO/Established
向第二组上报 Link
```

第三组负责：

```text
创建和删除 GRE/ip6gre
配置 overlay 地址
设置接口状态
提供虚接口 ifindex
报告 DEVICE_UP/READY/FAILED/DOWN
处理 Zebra 重连和内核设备消失
```

第三组不应：

- 自己决定是否建立 MIDR 邻接；
- 自己分配第一组的逻辑 `link_id`；
- 直接调用 `midr_session_connect()`；
- 直接调用 `midr_topology_link_upsert()`；
- 将 GRE 外层地址作为 MIDR Link 的 overlay 下一跳；
- 要求第一组通过 shell 配置设备。

## 8. Session 实际选路检查

当前 `midr_session_connect()` 接收远端 endpoint，但没有单独传入本地 ifindex 或源地址；transport 建立出站连接时使用 `midrd` 的本地监听配置进行 bind。

因此第三组不能仅凭返回 `READY` 保证 MIDR Session 会经 GRE。第三组需要向第一组返回完整设备和 overlay 状态；第一组、第二组还需要联合确认：

```text
midrd 监听地址是否覆盖 overlay 地址；
出站连接源地址是否正确；
到 overlay_remote 的 route get 是否显示 GRE 设备；
物理 Link 和 Overlay Link 并存时是否能选择正确 endpoint；
对端 HELLO 的身份准入是否正确。
```

如果现有单一监听地址无法满足物理和 overlay 两类邻接，需要由第一组、第二组另行提出 Session API 或 runtime 调整；这不是 GRE 设备 API 单独能够解决的问题。

## 9. 测试要求

### 9.1 组件测试

补充覆盖：

- 缺失 outer endpoint；
- 地址族不一致；
- overlay 地址或 prefix 非法；
- Zebra 创建失败；
- overlay 地址配置失败；
- 接口置 UP 失败；
- READY 前超时；
- 重复 add 幂等；
- 相同 ifname 不同 endpoint 拒绝或显式 rebind；
- 删除和重建；
- 设备消失后的状态通知；
- ifindex 变化后状态正确更新。

### 9.2 GRE/IP 隧道测试

现有 `midr-test/midr-gre-connectivity-test.sh` 中手工执行的：

```bash
ip addr add ... dev greX
ip link set greX up
```

应改为通过第三组新 API 完成，并检查：

```text
设备类型正确（gre/ip6gre）
设备状态为 READY
overlay 地址已配置
overlay 双向 ping 成功
返回 ifindex 有效
删除后设备和地址均清理
```

### 9.3 跨 BGP-only 区域验收

拓扑：

```text
MIDR-A ---- BGP-only underlay ---- MIDR-B
```

第三组验收必须证明：

1. outer endpoint 经 underlay 可达；
2. 两端 GRE/ip6gre 设备都进入 READY；
3. overlay 地址和接口状态正确；
4. `route get overlay-remote` 选择虚接口；
5. 返回的 ifindex 可交给第一组和第二组；
6. Zebra/FIB 能使用该 ifindex 安装业务路由；
7. 删除、Zebra 重连、设备重建后状态可恢复。

第三组不需要在该测试中决定 MIDR 节点是否建邻；第一组负责调用 Session，并由联合测试验证最终 MIDR Session/EOR/SPF/FIB 闭环。

## 10. 完成标准

第三组完成以下内容后，才能认为虚链路设备接口交付：

```text
1. 提供可被第一组直接调用的高层虚链路 API；
2. API 不要求第一组执行 shell 或 raw netlink；
3. API 完成 GRE 创建、overlay 地址配置和接口 UP；
4. READY 状态明确表示设备、地址和本地数据面配置完成；
5. 能返回稳定有效的虚接口 ifindex；
6. 支持删除、故障通知、Zebra 重连和重建；
7. 组件测试覆盖失败回滚、幂等和状态转换；
8. GRE/IP6GRE 测试不再依赖外部手工地址配置；
9. 跨 BGP-only underlay 的联合验收能够证明实际出口为 GRE 虚接口；
10. 与第一组明确 READY、FAILED、DOWN、ifindex 和 overlay 地址的接口语义。
```
