<script setup lang="ts">
import { ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { adminsApi } from '@/api'
import { useAuth } from '@/stores/auth'
import { adminRoleTone, adminStateLabel, fmtShort } from '@/lib/console'
import { useSettingsShell } from '@/lib/settingsShell'
import type { Admin } from '@/api/types'

type AdminRole = Admin['role']

const roleOptions: Array<{ value: AdminRole, label: string }> = [
  { value: 'operator', label: '操作员' },
  { value: 'admin', label: '管理员' },
  { value: 'viewer', label: '访客' },
]

const router = useRouter()
const auth = useAuth()

const createMode = ref(false)
const admins = ref<Admin[]>([])
const form = ref<{ username: string, password: string, role: AdminRole }>({
  username: '',
  password: '',
  role: 'operator',
})

function roleLabel(role: AdminRole) {
  return roleOptions.find(option => option.value === role)?.label || role
}

const { loading, reload: load } = useAsyncLoader(async () => {
  const response = await adminsApi.list()
  admins.value = response.admins
}, {
  streamEvents: ['admin.created', 'admin.updated', 'admin.deleted', 'admin.login'],
  streamDebounceMs: 180,
})

async function createAdmin() {
  try {
    await adminsApi.create(form.value)
    ElMessage.success('创建成功')
    createMode.value = false
    form.value = { username: '', password: '', role: 'operator' }
    await load()
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '创建失败')
  }
}

useSettingsShell(() => {
  if (!auth.isAdmin) {
    return []
  }

  if (createMode.value) {
    return [
      { key: 'cancel', label: '取消', tone: 'text' as const, action: () => { createMode.value = false } },
      { key: 'create', label: '创建', action: createAdmin },
    ]
  }

  return [
    { key: 'new', label: '新增', action: () => { createMode.value = true } },
  ]
})
</script>

<template>
  <div class="settings-stack">
    <div class="hero-title">操作员</div>

    <div v-if="!auth.isAdmin" class="empty-copy">
      <strong>没有权限</strong>
      <span>只有管理员可以查看和管理操作员</span>
    </div>

    <template v-else>
      <div v-if="createMode" class="create-box">
        <div class="role-picker">
          <span class="role-label">角色</span>
          <div class="role-options">
            <button
              v-for="option in roleOptions"
              :key="option.value"
              type="button"
              class="role-option"
              :class="{ active: form.role === option.value }"
              @click="form.role = option.value"
            >
              {{ option.label }}
            </button>
          </div>
        </div>
        <div class="field-row">
          <span>账号</span>
          <input v-model="form.username" class="plain-input" />
        </div>
        <div class="field-row">
          <span>密码</span>
          <input v-model="form.password" type="password" class="plain-input" />
        </div>
      </div>

      <div v-else-if="admins.length" class="admin-list">
        <button
          v-for="item in admins"
          :key="item.username"
          type="button"
          class="admin-row"
          @click="router.push({ name: 'settings-admin-detail', params: { username: item.username } })"
        >
          <span class="col-name">{{ item.username }}</span>
          <span class="col-mid" :class="adminRoleTone(item.role)">{{ roleLabel(item.role) }}</span>
          <span class="col-status">{{ adminStateLabel(item) }}</span>
          <span class="col-time">{{ fmtShort(item.last_login_at) }}</span>
        </button>
      </div>
      <div v-else class="empty-copy">
        <strong>暂无操作员</strong>
        <span>点击右下角创建第一个账户</span>
      </div>
    </template>
  </div>
</template>

<style scoped>
.settings-stack { display: grid; gap: 8px; min-height: 100%; }
.create-box, .admin-list { display: grid; gap: 12px; }
.role-picker { display: grid; gap: 10px; padding: 0 0 6px; }
.role-label { font-size: 14px; color: var(--bo-console-text-soft); }
.role-options { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
.role-option {
  min-height: 42px;
  padding: 10px 14px;
  border: 2px solid var(--bo-console-line);
  border-radius: 14px;
  background: none;
  color: var(--bo-console-text-soft);
  font: inherit;
  font-size: 16px;
  font-weight: 600;
  cursor: pointer;
}
.role-option.active {
  border-color: var(--bo-console-shell);
  background: var(--bo-console-shell);
  color: var(--bo-console-accent-fg);
}
.field-row {
  --bo-console-field-label-font: var(--bo-console-font-control);
  --bo-console-field-label-font-mobile: var(--bo-console-font-body);
}
.admin-row { display: grid; grid-template-columns: minmax(0, 1fr) 72px 54px 108px; align-items: baseline; gap: 8px; padding: 0; border: none; background: none; color: inherit; text-align: left; font: inherit; cursor: pointer; }
.col-name { display: block; gap: 2px; }
.col-name small, .col-time { color: var(--bo-console-text-faint); font-size: 13px; }
.col-time { justify-self: end; text-align: right; }
@container beacon-shell (max-width: 900px) {
  .role-options { gap: 8px; }
  .role-option { min-height: 38px; padding: 8px 10px; font-size: 14px; }
  .admin-row { grid-template-columns: minmax(0, 1fr) auto auto; gap: 4px 10px; }
  .admin-row .col-name { grid-column: 1 / -1; }
  .admin-row .col-time { justify-self: start; text-align: left; }
}
</style>
