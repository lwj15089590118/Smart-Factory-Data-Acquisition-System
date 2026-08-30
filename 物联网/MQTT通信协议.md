# MQTT 通信协议规范

> 项目：Smart-Factory-Data-Acquisition-System
> 版本：v1.2（与节点固件 v1.2.0、看板 dashboard.py 配套）
> 协议：MQTT 3.1.1，Broker 推荐 EMQX / mosquitto，默认端口 1883（TLS 加固用 8883）

---

## 1. 总体原则

| 项目 | 约定 |
|------|------|
| ClientID | 节点：`node1`；看板：`dashboard-server`；命名唯一，防止互踢 |
| 用户认证 | 用户名 `sfda` / 密码 `sfda123`（仅供本地演示，看板侧可用环境变量 `MQTT_USER / MQTT_PASS` 注入覆盖，生产环境务必改为强口令） |
| QoS | 数据上行 QoS1（至少一次）；命令下行 QoS1；状态主题 retain=1 |
| 遗嘱 LWT | 节点掉线时由 Broker 代发 `offline`，看板据此显示离线 |
| 心跳 | keepalive=30s；节点 30s 未连上 Broker 自动重连 |
| 时间戳 | 节点无 RTC，用 `uptime`（上电秒数）；以 `time` 开头的字段为上位机 ISO 时间 |

## 2. 主题（Topic）设计

主题采用分层结构：`factory / {deviceId} / {类别}`

| 主题 | 方向 | QoS | Retain | 说明 |
|------|------|-----|--------|------|
| `factory/node1/data` | 节点→看板 | 1 | 否 | 周期采集数据（5s 一条） |
| `factory/node1/alarm` | 节点/看板→订阅者 | 1 | 否 | 报警事件（触发沿发布） |
| `factory/node1/status` | 节点→看板 | 1 | 是 | 在线状态（online/offline，LWT） |
| `factory/node1/cmd` | 看板→节点 | 1 | 否 | 命令下行（设阈值/消音/重启） |
| `factory/node1/cmd_resp` | 节点→看板 | 1 | 否 | 命令执行回执 |
| `factory/dashboard/status` | 看板→Broker | 1 | 是 | 看板自身在线状态 |

通配订阅：上位机监控所有节点可订阅 `factory/+/data`、`factory/+/alarm`、`factory/+/status`。

## 3. 数据格式（JSON）

### 3.1 采集数据 `factory/node1/data`（每 5 秒）

```json
{
  "deviceId": "node1",
  "temp": 26.8,
  "humi": 55.0,
  "current": 3.42,
  "voltage": 24.1,
  "alarm": 0,
  "uptime": 3617,
  "fw": "v1.2.0"
}
```

| 字段 | 类型 | 单位 | 说明 |
|------|------|------|------|
| deviceId | string | — | 设备编号 |
| temp | float | degC | DHT11 温度，传感器故障时为 -99.9 |
| humi | float | %RH | DHT11 湿度，故障时为 -1 |
| current | float | A | ACS712 电流，有符号，双向 |
| voltage | float | V | 分压采样电压 |
| alarm | int | 位图 | bit0 温度越限，bit1 电流越限，bit2 电压越限 |
| uptime | int | s | 节点上电秒数 |
| fw | string | — | 固件版本 |

### 3.2 报警事件 `factory/node1/alarm`

```json
{
  "deviceId": "node1",
  "alarm": 2,
  "temp": 41.3,
  "current": 11.25,
  "voltage": 24.0,
  "uptime": 5400,
  "source": "device",
  "time": "2026-08-20T14:23:05"
}
```

> `alarm=2`（bit1 置位）表示**电流越限**；`source=dashboard` 表示由看板二次判定触发。
> 节点侧告警报文（`source=device`）因无 RTC **不含 time 字段**，事件时间以看板接收时刻为准。

### 3.3 在线状态 `factory/node1/status`（retain=1）

```json
{ "deviceId": "node1", "state": "online", "fw": "v1.2.0" }
```

> 固件 status 报文不含 `uptime`（与固件 `MQTT_PublishStatus` 实际字段一致）。

掉线时 Broker 代发遗嘱：

```json
{ "deviceId": "node1", "state": "offline" }
```

### 3.4 下行命令 `factory/node1/cmd`

设置阈值（看板网页 → 节点）：

```json
{
  "id": 2026082201,
  "type": "set_threshold",
  "params": { "temp_max": 45.0, "curr_max": 12.0 },
  "time": "2026-08-22T09:30:00"
}
```

> `id` 由看板侧**自增生成**（1, 2, 3, …），固件回执 `cmd_resp` 原样带回，用于请求-回执对账。

命令回执 `factory/node1/cmd_resp`：

```json
{ "id": 2026082201, "result": 0, "msg": "ok" }
```

支持的其他命令：

| type | params | 说明 |
|------|--------|------|
| `set_threshold` | temp_max / curr_max / volt_max / volt_min | 写阈值并 Flash 保存 |
| `mute` | 无 | 蜂鸣器消音 |
| `reboot` | 无 | 节点软复位 |

## 4. 报警位图速查

| alarm 值 | 含义 |
|----------|------|
| 0 | 正常 |
| 1 | 温度越上限 |
| 2 | 电流越上限 |
| 4 | 电压越上/下限 |
| 3 | 温度+电流同时越限 |
| 7 | 三项同时越限 |

## 5. 可靠性与安全设计

1. **防重复/乱序**：QoS1 可能重复投递，看板按 `uptime` 去重，时间戳回退的报文直接丢弃；
2. **防误报**：节点与看板双侧均执行"连续 3 周期确认 + 5% 迟滞回差"才置报警位；
3. **掉线感知**：节点与看板均配置 LWT，任何一端异常退出，订阅方 1.5×keepalive 内收到 offline；
4. **自动重连**：ESP8266 侧 30s 重连 Broker；看板侧指数退避重连（2s→4s→…→60s 封顶）；
5. **安全加固（生产环境）**：启用 8883 TLS 双向认证、按设备分配独立账号、Broker 绑定内网网卡、EMQX 开启 ACL 限制每个 Client 只能发布自己的主题。
