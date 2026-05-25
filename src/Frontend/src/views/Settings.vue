<script setup lang="ts">
import { computed } from 'vue'
import { useRoute } from 'vue-router'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { provideSettingsShell } from '@/lib/settingsShell'
import { useAuth } from '@/stores/auth'

const route = useRoute()
const auth = useAuth()
const { footerActions } = provideSettingsShell()
const canManageSettings = computed(() => auth.permissions.canManageSettings)

const secondaryItems = computed(() => {
  if (!canManageSettings.value) return []
  const name = String(route.name || '')
  return [
    { key: 'admins', label: '操作员', to: { name: 'settings-admins' }, active: name.startsWith('settings-admin') },
    { key: 'audit', label: '审计', to: { name: 'settings-audit' }, active: name === 'settings-audit' },
    { key: 'about', label: '关于', to: { name: 'settings-about' }, active: name === 'settings-about' },
  ]
})
</script>

<template>
  <ConsoleShell
    active-main="settings"
    :secondary-items="secondaryItems"
    :footer-actions="canManageSettings ? footerActions : []"
    :title="canManageSettings ? '' : '当前无权限'"
  >
    <router-view v-if="canManageSettings" />
    <div v-else class="empty-copy settings-denied">
      <strong>无法访问设置</strong>
      <span>当前角色可以使用控制台的可见页面，但不能进入系统设置。</span>
    </div>
  </ConsoleShell>
</template>
