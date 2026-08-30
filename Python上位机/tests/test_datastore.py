# -*- coding: utf-8 -*-
"""DataStore.add 单元测试: 合法入库 / 数值范围 / NaN-Inf / QoS1 去重 / 时间戳回退"""
import pytest

import dashboard
from dashboard import DataStore


@pytest.fixture()
def store():
    return DataStore(maxlen=100)


def _payload(uptime=100, temp=26.8, humi=55.0, current=3.42, voltage=24.1, alarm=0):
    return {"deviceId": "node1", "temp": temp, "humi": humi, "current": current,
            "voltage": voltage, "alarm": alarm, "uptime": uptime, "fw": "v1.2.0"}


class TestAddValid:
    def test_add_valid_returns_true(self, store):
        assert store.add(_payload(uptime=100)) is True
        assert store.last_ts == 100.0
        assert store.latest()["temp"] == 26.8

    def test_add_updates_latest(self, store):
        store.add(_payload(uptime=100, temp=26.0))
        store.add(_payload(uptime=105, temp=27.0))
        assert store.latest()["temp"] == 27.0
        assert list(store.buf["ts"]) == [100.0, 105.0]

    def test_add_writes_csv(self, store):
        assert store.add(_payload(uptime=100)) is True
        with open(dashboard.Config.CSV_PATH, encoding="utf-8") as f:
            rows = f.read().strip().splitlines()
        assert len(rows) == 2                       # 表头 + 1 行数据
        assert rows[0].startswith("uptime,time")    # CSV 表头含 uptime/time
        assert rows[1].startswith("100.0")          # 首列为 uptime

    def test_alarm_defaults_zero(self, store):
        p = _payload(uptime=100)
        p.pop("alarm")
        assert store.add(p) is True
        assert store.latest()["alarm"] == 0


class TestRangeCheck:
    def test_temp_out_of_range_dropped(self, store):
        assert store.add(_payload(uptime=100, temp=150.0)) is False
        assert store.add(_payload(uptime=100, temp=-60.0)) is False

    def test_sensor_fault_sentinel_dropped(self, store):
        """固件 DHT11 故障哨兵 -99.9 超出物理范围, 被丢弃(审查 P2-1 现状口径)"""
        assert store.add(_payload(uptime=100, temp=-99.9, humi=-1)) is False

    def test_nan_inf_dropped(self, store):
        assert store.add(_payload(uptime=100, temp=float("nan"))) is False
        assert store.add(_payload(uptime=100, current=float("inf"))) is False
        assert store.add(_payload(uptime=100, voltage=float("-inf"))) is False

    def test_missing_field_dropped(self, store):
        p = _payload(uptime=100)
        del p["current"]
        assert store.add(p) is False

    def test_non_numeric_dropped(self, store):
        assert store.add(_payload(uptime=100, temp="hot")) is False

    def test_rejected_add_keeps_buffer(self, store):
        store.add(_payload(uptime=100))
        assert store.add(_payload(uptime=101, temp=150.0)) is False
        assert len(store.buf["ts"]) == 1
        assert store.last_ts == 100.0


class TestDedupAndRegression:
    def test_exact_duplicate_dropped(self, store):
        assert store.add(_payload(uptime=100)) is True
        assert store.add(_payload(uptime=100)) is False     # QoS1 重复投递
        assert len(store.buf["ts"]) == 1

    def test_reorder_within_tolerance_dropped(self, store):
        store.add(_payload(uptime=100))
        assert store.add(_payload(uptime=98)) is False      # 5s 容忍窗内乱序丢弃
        assert store.last_ts == 100.0

    def test_big_regression_accepted_as_reboot(self, store):
        """大幅回退视为节点重启, 接受入库(协议 §5.1)"""
        store.add(_payload(uptime=1000, temp=26.0))
        assert store.add(_payload(uptime=10, temp=27.0)) is True
        assert store.last_ts == 10.0
        assert store.latest()["temp"] == 27.0

    def test_first_sample_with_zero_uptime_accepted(self, store):
        assert store.add(_payload(uptime=0)) is True
