# Frontend

The BeaconOps web console, written in Vue 3 + Vite + TypeScript. It is an admin-facing surface, not a polished end-user product: device lists, message dispatch, batch views, admin settings, runtime status. It only does the UI — what is actually possible depends entirely on which APIs the backend exposes.

---

## Stack

- Vue 3.5
- Vite 6
- TypeScript 5
- Vue Router 4
- Pinia 2
- Element Plus 2
- Axios
- ECharts 5 + vue-echarts 7

---

## Directory Layout

```text
Frontend/
├── public/                Static assets
├── src/
│   ├── api/               API wrappers and types
│   ├── components/        Console UI components
│   ├── composables/       Reusable composition logic
│   ├── lib/               Console helper logic
│   ├── router/            Route definitions
│   ├── stores/            Pinia stores
│   ├── views/             Page views
│   ├── App.vue
│   ├── main.ts
│   └── styles.css
├── index.html
├── package.json
├── vite.config.ts
└── tsconfig.json
```

---

## Page Structure

Current routes are grouped into the following areas:

- Login: `/login`
- Console home: `/home`
- Message sending: `/send`
- Message history and detail: `/history`, `/history/:msgId`
- Device list and detail: `/devices`, `/devices/:deviceId` (detail page includes an ECharts behavior timeline chart: step count, activity intensity, and online/offline events)
- Batch list and detail: `/batches`, `/batches/:batchUuid`
- Settings: `/settings/*`
  - Admins (full CRUD)
  - Audit logs (filterable by actor, action type, and time range)
  - About

Realtime data comes from the backend SSE endpoint (`/beacon/api/v1/stream`). Message status changes, device health, and device behavior are all pushed through this channel — no polling.

---

## API and Deployment Conventions

The frontend talks to the backend using a relative API base:

```ts
export const API_BASE = '/beacon/api/v1'
```

The Vite production base path is:

```ts
base: '/beacon/console/'
```

This means the production build is expected to be served under `/beacon/console/`, while backend requests go through `/beacon/api/*`.

In development, the Vite dev server proxies `/beacon/api/*` to the local FastAPI server:

```ts
http://127.0.0.1:8020
```

---

## Local Development

Install dependencies:

```bash
npm install
```

Start the dev server:

```bash
npm run dev
```

Build for production:

```bash
npm run build
```

---

## Local Login Bypass

Development login bypass is available, but only when all of the following are true:

1. `import.meta.env.DEV === true`
2. The hostname is `localhost`, `127.0.0.1`, or `::1`
3. The local env file contains:

```env
VITE_BYPASS_LOGIN=true
```

You can copy `.env.development.local.example` to `.env.development.local` for local use.

This setting is for local development only and should never be committed.

---

## Boundary with the Backend

The frontend does not embed real accounts, passwords, or tokens. Authentication relies on backend session cookies, and mutating requests inject a CSRF header from the `beacon_csrf` cookie.

The open-source copy keeps reusable frontend source code and default configuration, while local-only environment files are excluded.
