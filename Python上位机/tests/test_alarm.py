# -*- coding: utf-8 -*-
"""AlarmManager 状态机测试: 连续确认 / 迟滞回差 / 恢复事件 / 多通道独立"""
import pytest

from dashboard import AlarmManager, DataStore


@pytest.fixture()
def mgr():
    return AlarmManager(DataStore(maxlen=10))


def _p(temp=26.0, humi=50.0, current=3.0, voltage=24.0):
    return {"temp": temp, "humi": humi, "current": current, "voltage": voltage}


class TestConfirmN:
    def test_two_overs_no_alarm(self, mgr):
        assert mgr.check(_p(current=11.0)) == []
        assert mgr.check(_p(current=11.0)) == []

    def test_three_consecutive_overs_fire(self, mgr):
        mgr.check(_p(current=11.0))
        mgr.check(_p(current=11.0))
        events = mgr.check(_p(current=11.0))
        assert len(events) == 1
        level, key, _msg = events[0]
        assert (level, key) == ("ALARM", "curr")

    def test_alarm_fires_only_once(self, mgr):
        for _ in range(4):
            events = mgr.check(_p(current=11.0))
        assert events == []                          # 已触发, 不重复发

    def test_counter_reset_by_normal_sample(self, mgr):
        mgr.check(_p(current=11.0))
        mgr.check(_p(current=11.0))
        mgr.check(_p(current=9.0))                   # 恢复正常, 计数清零
        mgr.check(_p(current=11.0))
        assert mgr.check(_p(current=11.0)) == []     # 不足 3 次, 不触发


class TestHysteresis:
    def test_recovery_in_hysteresis_band(self, mgr):
        for _ in range(3):
            mgr.check(_p(current=11.0))
        # 9.6 位于阈值*0.95=9.5 之上: 未恢复也不越限, 计数不增不翻转
        assert mgr.check(_p(current=9.6)) == []
        # 9.4 < 9.5: 恢复沿, 发 INFO
        events = mgr.check(_p(current=9.4))
        assert len(events) == 1 and events[0][0] == "INFO" and events[0][1] == "curr"

    def test_recovery_clears_state(self, mgr):
        for _ in range(3):
            mgr.check(_p(current=11.0))
        mgr.check(_p(current=9.0))                   # INFO 恢复
        assert mgr._fired == set()
        assert mgr._cnt["curr"] == 0

    def test_voltage_window_recover(self, mgr):
        mgr.check(_p(voltage=18.0))                  # 低于 volt_min=20
        mgr.check(_p(voltage=18.0))
        events = mgr.check(_p(voltage=18.0))         # 第 3 次确认触发
        assert events[0][1] == "volt"
        assert mgr.check(_p(voltage=25.0))[0][0] == "INFO"   # 回到 [21, 28.5] 区间

    def test_multiple_keys_independent(self, mgr):
        mgr.check(_p(temp=45.0, current=11.0))
        mgr.check(_p(temp=45.0, current=9.0))        # 电流恢复, 温度继续计数
        events = mgr.check(_p(temp=45.0, current=9.0))
        assert len(events) == 1
        assert events[0][1] == "temp"
