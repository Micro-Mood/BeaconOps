# Backend

The BeaconOps backend: FastAPI + SQLite + gmqtt. One process that speaks two protocols at the same time — HTTP to the frontend, MQTT to the devices. It handles admin login, batch and device management, message dispatch, MQTT bridging, audit logs, and realtime event streaming.

SQLite is a deliberate choice. The expected load for this project does not need Postgres, and any external dependency is one more thing to operate.

## Stack

- Python 3.11
- FastAPI
- aiosqlite
- gmqtt
- PyJWT
- bcrypt

## Directory Layout

```text
Backend/
├── app/
│   ├── main.py           FastAPI entry point
│   ├── config.py         Environment and path configuration
│   ├── database.py       SQLite schema and persistence helpers
│   ├── mqtt_client.py    Server-side MQTT bridge
│   ├── identity.py       Device HMAC authentication
│   ├── messages.py       Downlink message and retry flow
│   ├── routes/           HTTP API routes
│   └── ...
├── .env.example
├── requirements.txt
└── README_EN.md
```

## API Prefixes

The backend API is mounted under:

```text
/beacon/api/v1
```

Health check endpoint:

```text
/beacon/health
```

Main route groups:

- auth: admin login (Cookie + CSRF double protection via `csrf_guard` middleware), logout, current session, and device auth webhook
- batches: create, inspect, revoke (revocation asynchronously restarts the Mosquitto broker to actively disconnect currently-online devices), restore, and rotate batch secrets
- devices: device list and detail
- messages: send, list, and inspect delivery state
- admins: admin account management
- audit: audit log queries (filterable by actor, action type, and time range)
- stream: realtime event stream (message status changes, device health, and device behavior — all three channels via SSE)

## Data Model and Runtime Behavior

The database is created under `DATA_DIR`, which defaults to:

```text
./data/beacon.db
```

On startup, the app creates tables automatically and seeds a single admin account when `ADMIN_USERNAME` and `ADMIN_PASSWORD_HASH` are configured.

The `batch_secret` is not hardcoded in source files. It is generated at runtime when a batch is created or when its secret is rotated, then stored in the database.

Downlink message retries do not block the publish path: `messages.py` runs a `retry_loop` that schedules retries based on `next_retry_at`. When a device reconnects (`uplink_handler` receives `status=online`), `on_device_online` immediately re-pushes all messages still in `queued` or `sent` state — no waiting for the next retry window.

## MQTT Broker Integration

The backend connects to the broker using `MQTT_USERNAME` / `MQTT_PASSWORD` as the bridge identity and subscribes to these uplink topics:

- `device/+/status`
- `device/+/uplink/ack`
- `device/+/uplink/event`
- `device/+/uplink/health`
- `device/+/uplink/profile`

Device authentication endpoint:

```text
POST /beacon/api/v1/auth/device
```

If `AUTH_WEBHOOK_TOKEN` is empty, the webhook only accepts loopback requests.

## Local Development

Install dependencies:

```bash
pip install -r requirements.txt
```

Copy the environment template:

```bash
copy .env.example .env
```

Start the server:

```bash
python -m uvicorn app.main:app --host 127.0.0.1 --port 8020
```

If you want the backend to serve the built frontend bundle too, place the frontend build output under `Backend/console/`. `app/main.py` will expose `/beacon/console/` automatically when that directory exists.

## Removed from the Open-Source Copy

This open-source directory intentionally excludes:

- `.env`
- `.env.local`
- runtime databases and logs under `data/`
- local Python virtual environments and cache directories

You must provide your own admin password hash, JWT secret, MQTT bridge credentials, and optional webhook token.