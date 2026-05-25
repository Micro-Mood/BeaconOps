<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { adminsApi } from '@/api'
import { useAuth } from '@/stores/auth'
import { adminRoleTone, adminStateLabel, fmtDateTime } from '@/lib/console'
import { useSettingsShell } from '@/lib/settingsShell'
import type { Admin } from '@/api/types'

const route = useRoute()
const router = useRouter()
const auth = useAuth()

const username = computed(() => route.params.username as string)
const admin = ref<Admin | null>(null)
const passwordMode = ref(false)
const passwordInput = ref('')
const deleteMode = ref(false)

const { loading } = useAsyncLoader(async () => {
  const rows = (await adminsApi.list()).admins
  admin.value = rows.find(item => item.username === username.value) || null
}, {
  watchSources: username,
  streamEvents: ['admin.created', 'admin.updated', 'admin.deleted', 'admin.login'],
  streamFilter: event => event.username === username.value,
  streamDebounceMs: 180,
})

const isSelf = computed(() => admin.value?.username === auth.username)

async function savePassword() {
  if (!admin.value || passwordInput.value.length < 4) return
  try {
    await adminsApi.patch(admin.value.username, { password: passwordInput.value })
    passwordInput.value = ''
    passwordMode.value = false
    ElMessage.success('密码已更新')
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '设置失败')
  }
}

async function toggleEnabled() {
  if (!admin.value || isSelf.value) return
  try {
    await adminsApi.patch(admin.value.username, { enabled: !admin.value.enabled })
    admin.value.enabled = admin.value.enabled ? 0 : 1
    ElMessage.success('状态已更新')
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '操作失败')
  }
}

async function removeAdmin() {
  if (!admin.value || isSelf.value) return
  try {
    await adminsApi.delete(admin.value.username)
    ElMessage.success('已删除')
    router.push({ name: 'settings-admins' })
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '删除失败')
  }
}

useSettingsShell(() => ([
  { key: 'back', label: '返回', action: () => { router.push({ name: 'settings-admins' }) } },
]))
</script>

<template>
  <div v-if="admin" class="admin-detail">
    <div class="hero-name">{{ admin.username }}</div>
    <div class="hero-state" :class="adminRoleTone(admin.role)">{{ adminStateLabel(admin) }}</div>
    <div class="hero-meta">最近登录 {{ fmtDateTime(admin.last_login_at) }}</div>

    <div class="divider" />
    <div class="label">操作</div>

    <div class="action-copy">设置该操作员的登录密码。</div>
    <button class="action-btn" @click="passwordMode = !passwordMode">设置密码</button>
    <div v-if="passwordMode" class="password-box">
      <input v-model="passwordInput" class="plain-input" type="password" placeholder="至少 4 位" />
      <div class="inline-actions">
        <button class="action-btn" :disabled="passwordInput.length < 4" @click="savePassword">保存密码</button>
        <button class="action-btn" @click="passwordMode = false">取消</button>
      </div>
    </div>

    <div class="action-copy">暂时禁止该操作员登录，可随时恢复。</div>
    <button class="action-btn" :disabled="isSelf" @click="toggleEnabled">{{ admin.enabled ? '禁用账号' : '恢复账号' }}</button>
    <div v-if="isSelf" class="hint-text">无法禁用当前登录账号。</div>

    <div class="action-copy">删除该操作员（不可恢复）。</div>
    <button class="action-btn danger" :disabled="isSelf" @click="deleteMode = !deleteMode">删除操作员</button>
    <div v-if="isSelf" class="hint-text">无法删除当前登录账号。</div>
    <div v-if="deleteMode && !isSelf" class="inline-actions">
      <button class="action-btn danger" @click="removeAdmin">确认删除</button>
      <button class="action-btn" @click="deleteMode = false">取消</button>
    </div>

    <div class="divider" />
  <div class="tail-copy">{{ admin.username }} · 创建于 {{ fmtDateTime(admin.created_at) }}</div>
  </div>
</template>

<style scoped>
.admin-detail { display: grid; gap: 8px; min-height: 100%; }
.hero-name { font-size: 34px; font-weight: 700; }
.hero-state { font-size: 18px; }
.hero-meta, .tail-copy, .hint-text, .action-copy { color: var(--bo-console-text-soft); }
.password-box { display: grid; gap: 8px; --bo-console-input-font: 16px; }
.inline-actions { display: flex; gap: 16px; }
@container beacon-shell (max-width: 900px) { .hero-name { font-size: 24px; } }
</style>
