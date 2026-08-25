#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
============================================================================
 Smart-Factory-Data-Acquisition-System  Web 可视化看板
 功能：
   1. paho-mqtt 订阅 factory/node1/# 主题，实时接收 STM32 采集节点数据
   2. Flask 提供 Web 服务与 REST API（实时数据/历史曲线/统计/事件/阈值）
   3. 内嵌 ECharts 页面：温湿度/电流/电压实时曲线 + 电流仪表盘 + 报警事件表
   4. 报警判定（连续确认+迟滞，防误报），报警事件落盘并发布到 MQTT 报警主题
   5. pandas 做统计分析，支持一键导出 CSV
 启动：
   pip install -r requirements.txt
   python dashboard.py --broker 192.168.1.100 --port 5000
   浏览器访问 http://127.0.0.1:5000
============================================================================
"""
import argparse
import csv
import json
import math
import os
import threading
import time
from collections import deque
from datetime import datetime

import pandas as pd
import paho.mqtt.client as mqtt
from flask import Flask, Response, jsonify, render_template_string, request

# ============================================================
# 全局配置
# ============================================================
class Config:
    BROKER_HOST   = os.environ.get("MQTT_HOST", "127.0.0.1")
    BROKER_PORT   = int(os.environ.get("MQTT_PORT", 1883))
    MQTT_USER     = os.environ.get("MQTT_USER", "sfda")
    MQTT_PASS     = os.environ.get("MQTT_PASS", "sfda123")
    CLIENT_ID     = "dashboard-server"
    DEVICE_ID     = "node1"
    TOPIC_DATA    = f"factory/{DEVICE_ID}/data"
    TOPIC_ALARM   = f"factory/{DEVICE_ID}/alarm"
    TOPIC_STATUS  = f"factory/{DEVICE_ID}/status"
    TOPIC_CMD     = f"factory/{DEVICE_ID}/cmd"
    TOPIC_CMD_RESP = f"factory/{DEVICE_ID}/cmd_resp"
    HISTORY_LEN   = 7200          # 环形缓冲容量（约 2 小时, 5s/条）
    DATA_DIR      = os.path.join(os.path.dirname(__file__), "data")
    CSV_PATH      = os.path.join(DATA_DIR, "history.csv")
    TS_REORDER_TOLERANCE_S = 5.0  # uptime 回退容忍窗口: 其内视为 QoS1 重复/乱序丢弃
    DATA_TIMEOUT_S = float(os.environ.get("DATA_TIMEOUT_S", 15))  # 无数据判离线阈值(调试Day10)


# ============================================================
# 数据仓库：线程安全的环形缓冲 + 事件表 + CSV 落盘
# ============================================================
class DataStore:
    FIELDS = ("ts", "temp", "humi", "current", "voltage", "alarm")

    def __init__(self, maxlen: int = Config.HISTORY_LEN):
        self.lock = threading.Lock()
        self.buf = {f: deque(maxlen=maxlen) for f in self.FIELDS}
        self.last_ts = 0.0
        self.events = deque(maxlen=500)
        self._online = False          # Broker/状态主题驱动的在线标志
        self._timed_out = False       # 数据超时看门狗标志
        self.last_data_wall = None    # 最近一次收到 data 报文的墙钟时间
        os.makedirs(Config.DATA_DIR, exist_ok=True)

    @property
    def online(self) -> bool:
        """设备有效在线 = 链路在线 且 数据未超时"""
        with self.lock:
            return self._online and not self._timed_out

    @online.setter
    def online(self, v: bool) -> None:
        with self.lock:
            self._online = bool(v)

    def add(self, payload: dict) -> bool:
        """写入一条采集数据；数值非法/QoS1重复/时间戳回退的报文丢弃（防数据跳变）"""
        try:
            ts = float(payload.get("uptime", int(time.time())))
            point = {
                "temp":    float(payload["temp"]),
                "humi":    float(payload["humi"]),
                "current": float(payload["current"]),
                "voltage": float(payload["voltage"]),
                "alarm":   int(payload.get("alarm", 0)),
            }
        except (KeyError, TypeError, ValueError) as exc:
            print(f"[DATA] 非法报文被丢弃: {exc} -> {payload}")
            return False
        for k, v in point.items():                       # NaN/Inf 防混入
            if not math.isfinite(v):
                return False
        if point["temp"] < -50 or point["temp"] > 120:   # 物理范围校验
            return False
        with self.lock:
            if self.last_ts and ts <= self.last_ts:
                if self.last_ts - ts <= Config.TS_REORDER_TOLERANCE_S:
                    return False                         # QoS1 重复/乱序报文直接丢弃
                # 大幅回退视为节点重启, 接受并继续记录(协议 §5.1)
            self.buf["ts"].append(ts)
            for k, v in point.items():
                self.buf[k].append(v)
            self.last_ts = ts
        self.last_data_wall = time.time()
        self._append_csv(ts, point)
        return True

    def _append_csv(self, ts: float, point: dict) -> None:
        new_file = not os.path.exists(Config.CSV_PATH)
        with open(Config.CSV_PATH, "a", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            if new_file:
                w.writerow(["uptime", "time", *point.keys()])
            w.writerow([ts, datetime.now().strftime("%Y-%m-%d %H:%M:%S"), *point.values()])

    def latest(self) -> dict:
        with self.lock:
            if not self.buf["ts"]:
                return {}
            return {k: (list(v)[-1] if v else None) for k, v in self.buf.items()}

    def series(self, n: int = 600) -> dict:
        """返回最近 n 条曲线数据（ECharts 用）"""
        with self.lock:
            size = len(self.buf["ts"])
            start = max(0, size - n)
            return {k: list(v)[start:] for k, v in self.buf.items()}

    def dataframe(self) -> pd.DataFrame:
        with self.lock:
            df = pd.DataFrame(dict(self.buf))
        return df if not df.empty else pd.DataFrame(columns=list(self.FIELDS))

    def stats(self, minutes: int = 60) -> dict:
        """pandas 统计最近 N 分钟的四项指标"""
        df = self.dataframe()
        if df.empty:
            return {}
        # uptime 为相对秒，转成真实时间戳便于按时间过滤
        df["clock"] = datetime.now().timestamp() - (df["ts"].iloc[-1] - df["ts"])
        recent = df[df["clock"] >= datetime.now().timestamp() - minutes * 60]
        out = {}
        for col, name in (("temp", "温度degC"), ("humi", "湿度%RH"),
                          ("current", "电流A"), ("voltage", "电压V")):
            s = recent[col]
            out[col] = {
                "name": name,
                "max":  round(s.max(), 2),
                "min":  round(s.min(), 2),
                "mean": round(s.mean(), 2),
                "std":  round(s.std(), 3),          # 波动率, 越大说明数据越不稳
            }
        return out

    def add_event(self, level: str, source: str, message: str) -> None:
        with self.lock:
            self.events.append({
                "time": datetime.now().strftime("%H:%M:%S"),
                "level": level, "source": source, "message": message,
            })

    def start_watchdog(self) -> None:
        """数据超时看门狗: 超过 DATA_TIMEOUT_S 无 data 报文判离线并记 WARN,
        数据恢复时记 INFO（调试记录 Day10 措施, LWT 覆盖不到的'假在线'场景）"""
        def _loop():
            while True:
                time.sleep(2)
                lw = self.last_data_wall
                if lw is None:
                    continue                       # 尚未收到过任何数据
                timed_out = (time.time() - lw) > Config.DATA_TIMEOUT_S
                with self.lock:
                    prev, self._timed_out = self._timed_out, timed_out
                if timed_out and not prev:
                    self.add_event("WARN", "device",
                                   f"数据超时(>{int(Config.DATA_TIMEOUT_S)}s), 判定节点离线")
                elif prev and not timed_out:
                    self.add_event("INFO", "device", "节点数据恢复上报")
        threading.Thread(target=_loop, daemon=True, name="data-watchdog").start()

    def event_list(self, n: int = 50) -> list:
        with self.lock:
            return list(self.events)[::-1][:n]


# ============================================================
# 报警管理：连续确认 + 迟滞回差（与单片机端策略一致，双重防误报）
# ============================================================
class AlarmManager:
    CONFIRM_N  = 3      # 连续 3 个周期越限才触发
    HYSTERESIS = 0.05   # 5% 迟滞

    def __init__(self, store: DataStore):
        self.store = store
        self.th = {"temp_max": 40.0, "curr_max": 10.0,
                   "volt_max": 30.0, "volt_min": 20.0}
        self._cnt = {"temp": 0, "curr": 0, "volt": 0}
        self._fired = set()

    def check(self, p: dict) -> list:
        """返回本次触发的事件列表 [(级别, 来源, 文案)]"""
        events = []
        tests = [
            ("temp", p["temp"] > self.th["temp_max"],
             p["temp"] < self.th["temp_max"] * (1 - self.HYSTERESIS),
             f"温度越限 {p['temp']:.1f}degC > {self.th['temp_max']}degC"),
            ("curr", p["current"] > self.th["curr_max"],
             p["current"] < self.th["curr_max"] * (1 - self.HYSTERESIS),
             f"电流越限 {p['current']:.2f}A > {self.th['curr_max']}A"),
            ("volt", p["voltage"] > self.th["volt_max"] or p["voltage"] < self.th["volt_min"],
             self.th["volt_min"] * (1 + self.HYSTERESIS) < p["voltage"] < self.th["volt_max"] * (1 - self.HYSTERESIS),
             f"电压越限 {p['voltage']:.1f}V (允许 {self.th['volt_min']}~{self.th['volt_max']}V)"),
        ]
        for key, over, recover, msg in tests:
            if over:
                self._cnt[key] += 1
                if self._cnt[key] >= self.CONFIRM_N and key not in self._fired:
                    self._fired.add(key)
                    events.append(("ALARM", key, msg))
            elif recover and key in self._fired:
                self._fired.discard(key)
                self._cnt[key] = 0
                events.append(("INFO", key, f"{key} 报警已恢复"))
            elif recover:
                self._cnt[key] = 0
        return events


# ============================================================
# MQTT 客户端
# ============================================================
class MqttBridge:
    def __init__(self, store: DataStore, alarm: AlarmManager):
        self.store, self.alarm = store, alarm
        self.client = mqtt.Client(client_id=Config.CLIENT_ID, clean_session=False)
        if Config.MQTT_USER:
            self.client.username_pw_set(Config.MQTT_USER, Config.MQTT_PASS)
        # 遗嘱：看板异常掉线时 Broker 自动发布 offline
        self.client.will_set("factory/dashboard/status",
                             json.dumps({"state": "offline"}), qos=1, retain=True)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self._mqtt_pub_lock = threading.Lock()

    def _on_connect(self, client, userdata, flags, rc):
        print(f"[MQTT] 已连接 Broker, rc={rc}")
        client.subscribe([(Config.TOPIC_DATA, 1), (Config.TOPIC_ALARM, 1),
                          (Config.TOPIC_STATUS, 1), (Config.TOPIC_CMD_RESP, 1)])
        client.publish("factory/dashboard/status",
                       json.dumps({"state": "online"}), qos=1, retain=True)
        self.store.online = True

    def _on_disconnect(self, client, userdata, rc):
        self.store.online = False
        self.store.add_event("WARN", "mqtt", f"与 Broker 连接断开 rc={rc}, 自动重连中")

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            print(f"[MQTT] 无法解析: {msg.topic} -> {msg.payload!r}")
            return
        if msg.topic == Config.TOPIC_DATA:
            if self.store.add(payload):
                for ev in self.alarm.check({k: payload[k] for k in
                                            ("temp", "humi", "current", "voltage")}):
                    level, source, text = ev
                    self.store.add_event(level, source, text)
                    if level == "ALARM":                       # 转发看板侧报警
                        self.publish(Config.TOPIC_ALARM, {
                            "deviceId": Config.DEVICE_ID, "source": "dashboard",
                            "message": text, "time": datetime.now().isoformat()})
        elif msg.topic == Config.TOPIC_ALARM:
            self.store.add_event("ALARM", "device",
                                 f"节点报警 code={payload.get('alarm')} "
                                 f"T={payload.get('temp')} I={payload.get('current')}")
        elif msg.topic == Config.TOPIC_CMD_RESP:
            self.store.add_event("INFO", "cmd_resp",
                                 f"命令回执 id={payload.get('id')} "
                                 f"result={payload.get('result')} {payload.get('msg', '')}")
        elif msg.topic == Config.TOPIC_STATUS:
            state = payload.get("state", "unknown")
            self.store.online = (state == "online")
            self.store.add_event("INFO", "status", f"节点状态: {state}")

    def publish(self, topic: str, payload: dict):
        with self._mqtt_pub_lock:
            self.client.publish(topic, json.dumps(payload), qos=1)

    def start(self):
        """带指数退避的重连循环，独立线程运行"""
        def _loop():
            backoff = 2
            while True:
                try:
                    self.client.connect(Config.BROKER_HOST, Config.BROKER_PORT, keepalive=30)
                    self.client.loop_forever(retry_first_connection=True)
                except Exception as exc:
                    self.store.online = False
                    print(f"[MQTT] 连接失败: {exc}, {backoff}s 后重试")
                    time.sleep(backoff)
                    backoff = min(backoff * 2, 60)
                else:
                    backoff = 2
        threading.Thread(target=_loop, daemon=True, name="mqtt-bridge").start()


# ============================================================
# 前端页面（ECharts）
# ============================================================
PAGE = r"""
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>智能工厂数据采集看板</title>
<script src="https://cdn.jsdelivr.net/npm/echarts@5/dist/echarts.min.js"></script>
<style>
 body{background:#0d1b2a;color:#e0e6ed;font-family:Consolas,Microsoft YaHei;margin:0;padding:12px}
 h1{font-size:20px;margin:4px 0 12px}
 .grid{display:grid;grid-template-columns:repeat(2,1fr);gap:12px}
 .card{background:#13263b;border-radius:8px;padding:8px}
 .kpi{font-size:28px;font-weight:bold;color:#4fc3f7}
 table{width:100%;border-collapse:collapse;font-size:13px}
 th,td{border-bottom:1px solid #1d3a57;padding:4px 6px;text-align:left}
 .ALARM{color:#ff5252}.INFO{color:#69f0ae}.WARN{color:#ffd740}
 #thr input{width:80px;background:#0d1b2a;color:#e0e6ed;border:1px solid #2a4a6b}
 button{background:#2a4a6b;color:#fff;border:0;padding:4px 12px;border-radius:4px;cursor:pointer}
</style>
</head>
<body>
<h1>🏭 Smart-Factory 数据采集看板 <span id="net" style="font-size:14px"></span></h1>
<div class="grid">
 <div class="card" id="gauge"   style="height:240px"></div>
 <div class="card" id="thstats" style="height:240px;overflow:auto">
   <b>近 60 分钟统计 (pandas)</b><div id="stats"></div>
   <hr><b>阈值设置</b>
   <form id="thr">温度上限 <input name="temp_max" step="0.1"> degC
     电流上限 <input name="curr_max" step="0.1"> A<br>
     电压上限 <input name="volt_max" step="0.1"> V
     电压下限 <input name="volt_min" step="0.1"> V
     <button type="submit">下发</button></form>
   <a href="/api/export.csv" style="color:#4fc3f7">导出 CSV</a>
 </div>
 <div class="card" id="c_temp"  style="height:260px"></div>
 <div class="card" id="c_humi"  style="height:260px"></div>
 <div class="card" id="c_curr"  style="height:260px"></div>
 <div class="card" id="c_volt"  style="height:260px"></div>
 <div class="card" style="grid-column:1/3;height:220px;overflow:auto">
   <b>报警与事件</b>
   <table id="events"><tr><th>时间</th><th>级别</th><th>来源</th><th>内容</th></tr></table>
 </div>
</div>
<script>
const charts = {};
function mk(id, title, unit, color){
  const c = echarts.init(document.getElementById(id));
  charts[id]=c;
  c.setOption({title:{text:title,textStyle:{color:'#9fb3c8',fontSize:13}},
   tooltip:{trigger:'axis'},
   xAxis:{type:'category',data:[],axisLabel:{color:'#7a8ca0'}},
   yAxis:{type:'value',name:unit,axisLabel:{color:'#7a8ca0'}},
   grid:{left:50,right:20,top:30,bottom:40},
   dataZoom:[{type:'inside'},{type:'slider',height:14,bottom:6}],
   series:[{type:'line',showSymbol:false,smooth:true,itemStyle:{color:color},
            lineStyle:{width:2},data:[]}]});
}
mk('c_temp','温度曲线','degC','#ff7043');
mk('c_humi','湿度曲线','%RH','#26c6da');
mk('c_curr','电流曲线','A','#ab47bc');
mk('c_volt','电压曲线','V','#66bb6a');
const gauge = echarts.init(document.getElementById('gauge'));
gauge.setOption({series:[{type:'gauge',min:0,max:25,
  progress:{show:true,width:12},axisLabel:{color:'#7a8ca0'},
  detail:{valueAnimation:true,formatter:'{value} A',fontSize:26,color:'#4fc3f7'},
  data:[{value:0,name:'负载电流'}]}]});

async function refresh(){
  const r = await fetch('/api/realtime'); const d = await r.json();
  if(!d.series || !d.series.ts || !d.series.ts.length) return;
  const esc = s => String(s).replace(/[&<>"']/g,
      c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  document.getElementById('net').textContent =
      d.online ? '● MQTT已连接' : '○ 设备离线';
  const put=(id,key)=>{charts[id].setOption({xAxis:{data:d.series.ts.map(t=>t.toFixed(0))},
      series:[{data:d.series[key]}]});};
  put('c_temp','temp');put('c_humi','humi');put('c_curr','current');put('c_volt','voltage');
  gauge.setOption({series:[{data:[{value:d.latest.current,name:'负载电流'}]}]});
  const s = await (await fetch('/api/stats?minutes=60')).json();
  if(s.temp) document.getElementById('stats').innerHTML =
    Object.values(s).map(x=>`${x.name}: 最大${x.max} / 最小${x.min} /
     均值${x.mean} / 波动${x.std}`).join('<br>');
  const e = await (await fetch('/api/events')).json();
  document.getElementById('events').innerHTML = '<tr><th>时间</th><th>级别</th><th>来源</th><th>内容</th></tr>'
    + e.map(x=>`<tr><td>${esc(x.time)}</td><td class="${esc(x.level)}">${esc(x.level)}</td>
       <td>${esc(x.source)}</td><td>${esc(x.message)}</td></tr>`).join('');
}
document.getElementById('thr').onsubmit = async (ev)=>{
  ev.preventDefault();
  const f = new FormData(ev.target), body = {};
  f.forEach((v,k)=>body[k]=parseFloat(v));
  await fetch('/api/thresholds',{method:'POST',
    headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert('阈值已下发到节点');
};
setInterval(refresh, 2000); refresh();
</script>
</body></html>
"""


# ============================================================
# Flask 应用与 REST API
# ============================================================
app = Flask(__name__)
store = DataStore()
alarm = AlarmManager(store)
bridge = MqttBridge(store, alarm)


@app.route("/")
def index():
    return render_template_string(PAGE)


def _minutes_arg(default: int = 60) -> int:
    """解析并钳制 minutes 查询参数, 非法值回落默认, 防止 500/异常查询"""
    raw = request.args.get("minutes", default)
    try:
        m = int(raw)
    except (TypeError, ValueError):
        return default
    return max(1, min(m, 1440))


@app.route("/api/realtime")
def api_realtime():
    return jsonify(online=store.online, latest=store.latest(),
                   series=store.series(n=720))


@app.route("/api/history")
def api_history():
    minutes = _minutes_arg(60)
    df = store.dataframe()
    if df.empty:
        return jsonify(ts=[], temp=[], humi=[], current=[], voltage=[])
    return jsonify(df.tail(minutes * 12).to_dict(orient="list"))


@app.route("/api/stats")
def api_stats():
    minutes = _minutes_arg(60)
    return jsonify(store.stats(minutes))


@app.route("/api/events")
def api_events():
    return jsonify(store.event_list())


@app.route("/api/thresholds", methods=["GET", "POST"])
def api_thresholds():
    if request.method == "GET":
        return jsonify(alarm.th)
    body = request.get_json(silent=True) or {}
    changed = {}
    for k, v in body.items():
        # 注意 bool 是 int 子类, JSON true/false 需显式排除
        if k in alarm.th and isinstance(v, (int, float)) \
                and not isinstance(v, bool) and 0 < v <= 10000:
            alarm.th[k] = float(v)
            changed[k] = float(v)
    if not changed:
        return jsonify(error="参数非法"), 400
    # 同步下发到单片机节点（MQTT 命令主题）
    bridge.publish(Config.TOPIC_CMD,
                   {"type": "set_threshold", "params": changed,
                    "time": datetime.now().isoformat()})
    store.add_event("INFO", "web", f"阈值已修改: {changed}")
    return jsonify(ok=True, thresholds=alarm.th)


@app.route("/api/export.csv")
def api_export():
    df = store.dataframe()
    return Response(df.to_csv(index=False), mimetype="text/csv",
                    headers={"Content-Disposition": "attachment; filename=history.csv"})


# ============================================================
# 入口
# ============================================================
def main():
    parser = argparse.ArgumentParser(description="智能工厂数据采集看板")
    parser.add_argument("--broker", default=Config.BROKER_HOST, help="MQTT Broker 地址")
    parser.add_argument("--port", type=int, default=5000, help="Web 服务端口")
    args = parser.parse_args()
    Config.BROKER_HOST = args.broker
    store.start_watchdog()
    bridge.start()
    store.add_event("INFO", "system", "看板服务启动")
    print(f"[WEB] http://0.0.0.0:{args.port}  (broker={Config.BROKER_HOST})")
    app.run(host="0.0.0.0", port=args.port, debug=False, threaded=True)


if __name__ == "__main__":
    main()
