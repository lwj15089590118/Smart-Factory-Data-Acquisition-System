# -*- coding: utf-8 -*-
"""pytest 公共配置: 把上级目录加入 sys.path 以便 import dashboard,
并把 CSV/数据目录重定向到临时目录, 测试不污染运行时 data/"""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import dashboard  # noqa: E402


@pytest.fixture(autouse=True)
def _tmp_data_dir(tmp_path, monkeypatch):
    """每个测试独立的数据目录, DataStore 产生的 CSV 均写入 tmp_path"""
    data_dir = tmp_path / "data"
    monkeypatch.setattr(dashboard.Config, "DATA_DIR", str(data_dir))
    monkeypatch.setattr(dashboard.Config, "CSV_PATH", str(data_dir / "history.csv"))
    yield
