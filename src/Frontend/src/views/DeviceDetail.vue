<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import DeviceBehaviorPanel from '@/components/console/DeviceBehaviorPanel.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { devicesApi } from '@/api'
import { deviceTone, fmtDateTime, messageStatusLabel, messageTone, messageTitle } from '@/lib/console'
import { useAuth } from '@/stores/auth'
import type { Device, Message } from '@/api/types'

const route = useRoute()
const router = useRouter()
const auth = useAuth()

const deviceId = computed(() => route.params.deviceId as string)
const canMutate = computed(() => auth.permissions.canMutate)
const device = ref<Device | null>(null)
const recentMessages = ref<Message[]>([])
const aliasInput = ref('')
const deptInput = ref('')

const { loading } = useAsyncLoader(async () => {
  const info = await devicesApi.get(deviceId.value)
  const shouldSyncAlias = !device.value || aliasInput.value === (device.value.alias || '')
  const shouldSyncDept = !device.value || deptInput.value === (device.value.dept || '')
  device.value = info
  if (shouldSyncAlias) aliasInput.value = info.alias || ''
  if (shouldSyncDept) deptInput.value = info.dept || ''
  recentMessages.value = (await devicesApi.messages(deviceId.value, 10)).messages
}, {
  watchSources: deviceId,
  streamEvents: [
    'device.online',
    'device.offline',
    'device.health',
    'device.updated',
    'message.created',
    'message.recipient_update',
    'message.status_change',
    'message.event',
    'batch.revoked',
    'batch.restored',
    'batch.deleted',
  ],
  streamFilter: event => {
    const type = String(event.type || '')
    const current = device.value

    if (type === 'batch.deleted' && current && event.batch_uuid === current.batch_uuid) {
      void router.push({ name: 'devices' })
      return false
    }
    if (type.startsWith('device.')) {
      return event.device_id === deviceId.value
    }
    if (type === 'message.created') {
      if (!current) return false
      const targetKind = String(event.target_kind || '')
      const targetValue = String(event.target_value || '')
      return targetKind === 'all'
        || (targetKind === 'device' && targetValue === deviceId.value)
        || (targetKind === 'dept' && !!current.dept && targetValue === current.dept)
    }
    if (type === 'message.status_change') {
      return recentMessages.value.some(item => item.msg_id === event.msg_id)
    }
    if (type === 'message.recipient_update' || type === 'message.event') {
      return event.device_id === deviceId.value
    }
    return !!current && event.batch_uuid === current.batch_uuid
  },
  streamDebounceMs: 180,
})

async function saveAlias() {
  if (!device.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能修改设备备注')
    return
  }
  try {
    await devicesApi.patch(device.value.device_id, { alias: aliasInput.value })
    device.value.alias = aliasInput.value
    ElMessage.success('已保存')
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '保存失败')
  }
}

async function saveDept() {
  if (!device.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能修改设备部门')
    return
  }
  try {
    await devicesApi.patch(device.value.device_id, { dept: deptInput.value })
    device.value.dept = deptInput.value
    ElMessage.success('已保存')
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '保存失败')
  }
}
</script>

<template>
  <ConsoleShell
    active-main="devices"
    :tone="loading ? 'loading' : device ? deviceTone(device) : 'normal'"
    :title="device?.alias || '设备详情'"
  >
    <div v-if="device" class="detail-stack">
      <div class="detail-meta">
        <div class="meta-id">{{ device.device_id }}</div>
        <div class="meta-top">{{ device.online ? '在线' : '离线' }}　RSSI {{ device.rssi ?? '—' }}　电量 {{ device.battery ?? '—' }}</div>
        <div class="meta-top">部门 {{ device.dept || '—' }}　批次 {{ device.batch_uuid || '—' }}</div>
        <div class="meta-top">固件 {{ device.fw_version || '—' }}</div>
        <div class="meta-top">最后上报 {{ fmtDateTime(device.last_seen_at) }}</div>
        <DeviceBehaviorPanel :device-id="device.device_id" />
      </div>

      <div class="divider" />
      <div class="label">标识</div>
      <div class="field-row">
        <span>备注</span>
        <input v-model="aliasInput" class="plain-input" :readonly="!canMutate" />
        <button v-if="canMutate" class="field-action" @click="saveAlias">保存</button>
      </div>
      <div class="field-row">
        <span>部门</span>
        <input v-model="deptInput" class="plain-input" :readonly="!canMutate" />
        <button v-if="canMutate" class="field-action" @click="saveDept">保存</button>
      </div>

      <div class="divider" />
  <div class="label">近期消息（显示 10 条）</div>
      <div v-if="recentMessages.length" class="console-list message-list">
        <button
          v-for="item in recentMessages"
          :key="item.msg_id"
          type="button"
          class="console-list-row message-row"
          @click="router.push({ name: 'history-detail', params: { msgId: item.msg_id } })"
        >
          <span class="col-name">{{ messageTitle(item) }}</span>
          <span class="col-status" :class="messageTone(item.status)">{{ messageStatusLabel(item.status) }}</span>
          <span class="col-time">{{ fmtDateTime(item.created_at) }}</span>
        </button>
      </div>
      <div v-else class="empty-copy">
        <strong>还没有消息</strong>
        <span>这台设备暂时没有收到历史消息</span>
      </div>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'devices' })">返回</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.detail-stack { display: grid; gap: 8px; }
.detail-meta { display: grid; gap: 8px; min-width: 0; }
.meta-id { font-size: clamp(22px, 5.5cqi, 34px); line-height: 1.08; font-weight: 700; }
.divider { --bo-console-divider-margin: 10px 0 2px; }
.field-row { --bo-console-field-columns: 64px minmax(0, 1fr) auto; }
.message-row { --bo-console-row-columns: minmax(0, 1.3fr) 110px auto; }
.col-time { color: var(--bo-console-text-muted); }
</style>
