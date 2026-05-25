import { createRouter, createWebHistory } from 'vue-router'
import { useAuth } from '@/stores/auth'

const router = createRouter({
  history: createWebHistory(import.meta.env.BASE_URL),
  routes: [
    {
      path: '/login',
      component: () => import('@/views/AuthLayout.vue'),
      children: [
        { path: '', name: 'login', component: () => import('@/views/Login.vue') },
      ],
    },
    {
      path: '/',
      component: () => import('@/views/Layout.vue'),
      meta: { requiresAuth: true },
      children: [
        { path: '', redirect: '/home' },

        { path: 'home', name: 'home',
          component: () => import('@/views/Home.vue'),
          meta: { title: '首页' } },

        { path: 'send', name: 'send',
          component: () => import('@/views/Send.vue'),
          meta: { title: '发消息' } },

        { path: 'history', name: 'history',
          component: () => import('@/views/Messages.vue'),
          meta: { title: '历史' } },
        { path: 'history/:msgId', name: 'history-detail',
          component: () => import('@/views/MessageDetail.vue'),
          meta: { title: '历史详情' } },
        { path: 'messages', redirect: '/history' },
        { path: 'messages/:msgId', redirect: to => `/history/${to.params.msgId}` },

        { path: 'devices', name: 'devices',
          component: () => import('@/views/Devices.vue'),
          meta: { title: '设备' } },
        { path: 'devices/:deviceId', name: 'device-detail',
          component: () => import('@/views/DeviceDetail.vue'),
          meta: { title: '设备详情' } },

        { path: 'batches', name: 'batches',
          component: () => import('@/views/Batches.vue'),
          meta: { title: '批次' } },
        { path: 'batches/:batchUuid', name: 'batch-detail',
          component: () => import('@/views/BatchDetail.vue'),
          meta: { title: '批次详情' } },

        {
          path: 'settings',
          component: () => import('@/views/Settings.vue'),
          meta: { title: '设置' },
          children: [
            { path: '', redirect: '/settings/admins' },
            { path: 'admins',  name: 'settings-admins',
              component: () => import('@/views/settings/Admins.vue') },
            { path: 'admins/:username', name: 'settings-admin-detail',
              component: () => import('@/views/settings/AdminDetail.vue') },
            { path: 'audit',   name: 'settings-audit',
              component: () => import('@/views/settings/Audit.vue') },
            { path: 'about',   name: 'settings-about',
              component: () => import('@/views/settings/About.vue') },
          ],
        },
      ],
    },
    { path: '/:pathMatch(.*)*', redirect: '/home' },
  ],
})

router.beforeEach(async (to) => {
  const auth = useAuth()
  const requiresAuth = to.matched.some(record => record.meta.requiresAuth)
  if (!auth.ready && (requiresAuth || to.name === 'login')) await auth.refresh()
  if (requiresAuth && !auth.isLogged) {
    return { name: 'login', query: { r: to.fullPath } }
  }
  if (to.name === 'login' && auth.isLogged) {
    return { path: (to.query.r as string) || '/send' }
  }
})

export default router
