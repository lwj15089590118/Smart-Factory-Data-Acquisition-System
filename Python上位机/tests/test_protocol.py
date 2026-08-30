# -*- coding: utf-8 -*-
"""协议两端一致性回放测试:
1) cmd 方向: 看板构造的 JSON -> 纯 Python 参考解析器(复刻 stm32_code.c
   Cmd_Handle/Cmd_ParseNum/Cmd_ParseId 的字符串匹配顺序与取值范围)回放,
   断言回执结果与两端生效的阈值一致;
2) data 方向: 按固件 UploadData 的字段顺序/printf 格式复刻上行走文,
   断言看板 DataStore 能按协议解析入库;
3) alarm 方向: 固件告警报文与看板自回声报文的事件处理(审查 P1-2 回归)。
"""
import json
import re
import types

import pytest

import dashboard
from dashboard import Config, DataStore, MqttBridge


# ---------------------------------------------------------------- 参考解析器
# 复刻固件 Cmd_ParseNum: 在原始行中找 "<key>": 后的数值, 不存在返回 None(-1000 哨兵)
def fw_parse_num(line: str, key: str):
    m = re.search(r'"%s"' % key, line)
    if m is None:
        return None
    m2 = re.match(r"\s*:\s*([-+0-9.eE]+)", line[m.end():])
    return float(m2.group(1)) if m2 else None


def fw_parse_id(line: str) -> int:
    """复刻固件 Cmd_ParseId: 无 id 字段返回 0"""
    m = re.search(r'"id"', line)
    if m is None:
        return 0
    m2 = re.match(r"\s*:\s*([-+0-9]+)", line[m.end():])
    return int(m2.group(1)) if m2 else 0


# 固件 Cmd_Handle 的取值范围(与 stm32_code.c 一一对应)
FW_RANGES = {"temp_max": (10, 200), "curr_max": (1, 100),
             "volt_max": (5, 1000), "volt_min": (1, 1000)}


def fw_cmd_handle(line: str, th: dict) -> dict:
    """纯 Python 参考解析器: 按固件分支顺序(set_threshold/mute/reboot)回放, 返回回执"""
    resp = {"id": fw_parse_id(line), "result": 1, "msg": "unknown"}
    if "set_threshold" in line:
        resp["msg"] = "param"
        hit = False
        for key, (lo, hi) in FW_RANGES.items():
            v = fw_parse_num(line, key)
            if v is None or not (lo <= v <= hi):
                continue
            if key == "volt_min" and not (v < th["volt_max"]):
                continue
            th[key] = v
            hit = True
        if hit:
            resp["result"], resp["msg"] = 0, "ok"
    elif "mute" in line or "reboot" in line:
        resp["result"], resp["msg"] = 0, "ok"
    return resp


def fw_upload_json(temp, humi, current, voltage, alarm, uptime) -> str:
    """复刻固件 UploadData 的字段顺序与 printf 格式(%.1f/%.2f/%d)"""
    return ('{"deviceId":"node1","temp":%.1f,"humi":%.1f,'
            '"current":%.2f,"voltage":%.1f,"alarm":%d,"uptime":%d,'
            '"fw":"v1.2.0"}' % (temp, humi, current, voltage, alarm, uptime))


def fw_alarm_json(alarm, temp, current, voltage, uptime) -> str:
    """复刻固件 Alarm_Check 触发沿报文(source=device, 无 time 字段)"""
    return ('{"deviceId":"node1","source":"device","alarm":%d,'
            '"temp":%.1f,"current":%.2f,"voltage":%.1f,"uptime":%d}'
            % (alarm, temp, current, voltage, uptime))


@pytest.fixture()
def store():
    return DataStore(maxlen=100)


@pytest.fixture()
def captured(monkeypatch):
    """拦截 bridge.publish, 捕获看板实际下发的 cmd 报文"""
    box = []
    monkeypatch.setattr(dashboard.bridge, "publish",
                        lambda topic, payload: box.append((topic, payload)))
    return box


@pytest.fixture()
def client():
    return dashboard.app.test_client()


@pytest.fixture(autouse=True)
def _restore_dashboard_state():
    """测试前后保存/恢复看板全局阈值与 cmd id 序号, 避免用例间污染"""
    th_snapshot = dict(dashboard.alarm.th)
    seq_snapshot = dashboard.bridge._cmd_seq
    yield
    dashboard.bridge._cmd_seq = seq_snapshot
    dashboard.alarm.th.clear()
    dashboard.alarm.th.update(th_snapshot)


class TestCmdDirection:
    def test_threshold_cmd_id_increment(self, client, captured):
        for expected in (1, 2, 3):
            rv = client.post("/api/thresholds",
                             json={"temp_max": 45.0, "curr_max": 12.0})
            assert rv.status_code == 200
            topic, payload = captured[-1]
            assert topic == Config.TOPIC_CMD
            assert payload["id"] == expected        # 自增 id(协议 §3.4)
            assert payload["type"] == "set_threshold"

    def test_cmd_json_firmware_replay_ok(self, client, captured):
        """看板下发的合法阈值: 固件参考解析器回放 result=0, 两端生效值一致"""
        rv = client.post("/api/thresholds",
                         json={"temp_max": 45.0, "curr_max": 12.0})
        assert rv.status_code == 200
        topic, payload = captured[-1]
        th_fw = dict(dashboard.alarm.th)            # 固件侧默认阈值同款
        resp = fw_cmd_handle(json.dumps(payload), th_fw)
        assert resp["result"] == 0 and resp["msg"] == "ok"
        assert resp["id"] == payload["id"]
        # 两端一致性: 固件解析出的阈值 == 看板已生效的阈值
        assert th_fw["temp_max"] == dashboard.alarm.th["temp_max"] == 45.0
        assert th_fw["curr_max"] == dashboard.alarm.th["curr_max"] == 12.0

    def test_cmd_json_firmware_replay_rejects_fw_out_of_range(self, client, captured):
        """看板范围[0,10000]宽于固件[10,200]: temp_max=5 看板放行, 固件拒绝(已知口径,
        见审查报告 P2 '阈值两端分叉'); 此处断言固件侧按协议拒绝并回执 param"""
        rv = client.post("/api/thresholds", json={"temp_max": 5.0})
        assert rv.status_code == 200
        _, payload = captured[-1]
        resp = fw_cmd_handle(json.dumps(payload), dict(dashboard.alarm.th))
        assert resp["result"] == 1 and resp["msg"] == "param"

    def test_cmd_without_id_replayed_as_zero(self):
        """无 id 的历史报文: 固件参考解析器按 0 回执(与 Cmd_ParseId 一致)"""
        resp = fw_cmd_handle('{"type":"mute"}', dict(dashboard.alarm.th))
        assert resp["id"] == 0 and resp["result"] == 0

    def test_cmd_unknown_type(self):
        resp = fw_cmd_handle('{"id":9,"type":"set_upload","params":{"interval":10}}',
                             dict(dashboard.alarm.th))
        assert resp == {"id": 9, "result": 1, "msg": "unknown"}   # set_upload 已从协议删除


class TestDataDirection:
    def test_firmware_upload_json_replay(self, store):
        """固件格式上行走文(字段顺序/格式化) -> 看板入库, 数值逐字段一致"""
        raw = fw_upload_json(26.8, 55.0, 3.42, 24.1, 0, 3617)
        payload = json.loads(raw)
        # 字段顺序与协议 §3.1 示例一致
        assert list(payload.keys()) == ["deviceId", "temp", "humi", "current",
                                        "voltage", "alarm", "uptime", "fw"]
        assert payload["alarm"] == 0 and isinstance(payload["alarm"], int)
        assert store.add(payload) is True
        latest = store.latest()
        assert latest["temp"] == pytest.approx(26.8)
        assert latest["current"] == pytest.approx(3.42)
        assert latest["voltage"] == pytest.approx(24.1)
        assert store.last_ts == 3617.0

    def test_firmware_upload_negative_current(self, store):
        """ACS712 双向电流 %.2f 带符号, 看板可解析"""
        payload = json.loads(fw_upload_json(26.0, 50.0, -1.25, 24.0, 0, 10))
        assert store.add(payload) is True
        assert store.latest()["current"] == pytest.approx(-1.25)


class TestAlarmDirection:
    def _msg(self, topic, raw):
        return types.SimpleNamespace(topic=topic, payload=raw.encode("utf-8"))

    def test_device_alarm_creates_event(self):
        bridge = MqttBridge(DataStore(maxlen=10), dashboard.alarm)
        before = len(bridge.store.event_list(1000))
        bridge._on_message(None, None,
                           self._msg(Config.TOPIC_ALARM,
                                     fw_alarm_json(2, 41.3, 11.25, 24.0, 5400)))
        events = bridge.store.event_list(1000)
        assert len(events) == before + 1
        assert events[0]["level"] == "ALARM" and events[0]["source"] == "device"
        assert "code=2" in events[0]["message"]

    def test_dashboard_echo_skipped(self):
        """看板自发布报警(source=dashboard)回流不得产生假事件(审查 P1-2)"""
        bridge = MqttBridge(DataStore(maxlen=10), dashboard.alarm)
        before = len(bridge.store.event_list(1000))
        echo = json.dumps({"deviceId": "node1", "source": "dashboard",
                           "message": "电流越限", "time": "2026-08-30T12:00:00"})
        bridge._on_message(None, None, self._msg(Config.TOPIC_ALARM, echo))
        assert len(bridge.store.event_list(1000)) == before   # 无新增事件
