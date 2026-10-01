"""Site-specific settings for the test/analysis tools, kept out of git.

Copy local.example.json to local.json and fill in your setup. Tools read it through get(); every
value can also be passed on the command line where the tool offers an option.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
_PATH = os.path.join(HERE, "local.json")
_cfg = None


def get(key, default=None):
    global _cfg
    if _cfg is None:
        _cfg = json.load(open(_PATH)) if os.path.exists(_PATH) else {}
    v = _cfg.get(key, default)
    if isinstance(v, str) and v.startswith("./"):          # paths relative to the firmware root
        v = os.path.normpath(os.path.join(HERE, "..", v[2:]))
    return v


def need(key):
    v = get(key)
    if v in (None, ""):
        raise SystemExit(f"tools/local.json: '{key}' is not set (see tools/local.example.json)")
    return v
