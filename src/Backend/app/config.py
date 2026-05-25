"""环境变量加载。所有可调项集中此处。"""
from __future__ import annotations
import os
from pathlib import Path
from dotenv import load_dotenv

load_dotenv(Path(__file__).resolve().parent.parent / ".env")


def _int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except (TypeError, ValueError):
        return default


def _bool(name: str, default: bool) -> bool:
    v = os.getenv(name, "").strip().lower()
    if v in ("1", "true", "yes", "on"):
        return True
    if v in ("0", "false", "no", "off", ""):
        return default if v == "" else False
    return default


# —— 路径 ——————————————————————————————————————
DATA_DIR = Path(os.getenv("DATA_DIR", "./data")).resolve()
DATA_DIR.mkdir(parents=True, exist_ok=True)
DB_PATH = DATA_DIR / "beacon.db"

# —— 管理员种子 ————————————————————————————————
ADMIN_USERNAME      = os.getenv("ADMIN_USERNAME", "admin")
ADMIN_PASSWORD_HASH = os.getenv("ADMIN_PASSWORD_HASH", "")

# —— 服务信息 ————————————————————————————————
ENVIRONMENT = os.getenv("ENVIRONMENT", os.getenv("APP_ENV", "production"))

# —— JWT ——————————————————————————————————————
JWT_SECRET = os.getenv("JWT_SECRET", "change-me")
JWT_ALG    = "HS256"
JWT_TTL_S  = 8 * 3600

# —— MQTT ——————————————————————————————————————
MQTT_HOST     = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT     = _int("MQTT_PORT", 1883)
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")
MQTT_CLIENT_ID = "beacon-bridge"

# —— 设备接入鉴权 ————————————————————————————
HMAC_MAX_SKEW_S    = _int("HMAC_MAX_SKEW_S", 300)
NONCE_TTL_S        = _int("NONCE_TTL_S", 600)
AUTH_WEBHOOK_TOKEN = os.getenv("AUTH_WEBHOOK_TOKEN", "")  # 共享令牌;留空时仅允许本机直连 webhook

# —— 下行重试 ————————————————————————————————
DOWNLINK_MAX_ATTEMPTS    = _int("DOWNLINK_MAX_ATTEMPTS", 10)
DOWNLINK_BACKOFF_BASE_S  = _int("DOWNLINK_BACKOFF_BASE_S", 2)
DOWNLINK_BACKOFF_MAX_S   = _int("DOWNLINK_BACKOFF_MAX_S", 300)
DOWNLINK_RETRY_TICK_S    = 1

# —— 健康历史 ————————————————————————————————
HEALTH_HISTORY_ENABLED = _bool("HEALTH_HISTORY_ENABLED", False)
