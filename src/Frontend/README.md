# Frontend

BeaconOps 的 Web 控制台，Vue 3 + Vite + TypeScript 写的。功能上就是后台能看设备、能发消息、能查批次、能改设置、能看运行状态，不是什么面向用户的产品页面。它只负责 UI，实际能力都看后端提供了哪些 API。

---

## 技术栈

- Vue 3.5
- Vite 6
- TypeScript 5
- Vue Router 4
- Pinia 2
- Element Plus 2
- Axios
- ECharts 5 + vue-echarts 7

---

## 目录结构

```text
Frontend/
├── public/                静态资源
├── src/
│   ├── api/               API 封装与类型定义
│   ├── components/        控制台 UI 组件
│   ├── composables/       组合式逻辑
│   ├── lib/               控制台辅助逻辑
│   ├── router/            路由配置
│   ├── stores/            Pinia 状态管理
│   ├── views/             页面视图
│   ├── App.vue
│   ├── main.ts
│   └── styles.css
├── index.html
├── package.json
├── vite.config.ts
└── tsconfig.json
```

---

## 页面结构

当前路由大致分为以下几类：

- 登录页：`/login`
- 控制台首页：`/home`
- 消息发送：`/send`
- 消息历史与详情：`/history`、`/history/:msgId`
- 设备列表与详情：`/devices`、`/devices/:deviceId`（详情页含 ECharts 行为时间轴图：步数、活动强度、在线/离线节点）
- 批次列表与详情：`/batches`、`/batches/:batchUuid`
- 设置页：`/settings/*`
  - 管理员（增删改查）
  - 审计日志（按操作人、操作类型、时间范围过滤）
  - 关于页

实时数据来自后端 SSE（`/beacon/api/v1/stream`），消息状态变化、设备 health、设备行为三路均从这里推送，不轮询。

---

## API 与部署约定

前端默认通过相对路径访问后端：

```ts
export const API_BASE = '/beacon/api/v1'
```

Vite 构建基路径为：

```ts
base: '/beacon/console/'
```

这意味着生产环境默认部署在站点的 `/beacon/console/` 路径下，并通过 `/beacon/api/*` 访问后端接口。

开发环境下，Vite dev server 会把 `/beacon/api/*` 代理到本地 FastAPI：

```ts
http://127.0.0.1:8020
```

---

## 本地开发

安装依赖：

```bash
npm install
```

启动开发服务器：

```bash
npm run dev
```

构建生产版本：

```bash
npm run build
```

---

## 本地绕过登录

开发环境支持本地登录绕过，但只有在以下条件同时满足时才会启用：

1. `import.meta.env.DEV === true`
2. 主机名是 `localhost` / `127.0.0.1` / `::1`
3. 本地环境文件中设置了：

```env
VITE_BYPASS_LOGIN=true
```

可以复制 `.env.development.local.example` 为 `.env.development.local` 后使用。

该配置仅用于本地开发，不应提交到版本库。

---

## 与后端的边界

该前端不内嵌真实账号、密码或 Token。认证依赖后端 Session / Cookie，写操作通过 `beacon_csrf` Cookie 注入 CSRF 头。

开源前已去除本地开发环境文件，保留的是可复用的前端源码与默认配置。
