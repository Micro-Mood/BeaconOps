<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { devicesApi, messagesApi } from '@/api'
import type { SendBody } from '@/api'
import { levelLabel, levelTone } from '@/lib/console'
import type { Device, Level } from '@/api/types'
import { useAuth } from '@/stores/auth'

const router = useRouter()
const auth = useAuth()

type Stage = 'target' | 'compose'
type TargetKind = 'device' | 'dept' | 'all'

const stage = ref<Stage>('target')
const targetKind = ref<TargetKind>('device')
const level = ref<Level>('info')
const title = ref('')
const body = ref('')
const sending = ref(false)
const devices = ref<Device[]>([])
const selectedDeviceIds = ref<string[]>([])
const selectedDept = ref('')
const deviceQuery = ref('')
const deptQuery = ref('')

const { loading } = useAsyncLoader(async () => {
  devices.value = (await devicesApi.list({ enabled: 1 })).devices
}, {
  streamEvents: ['device.online', 'device.offline', 'device.updated', 'batch.revoked', 'batch.restored', 'batch.deleted'],
  streamDebounceMs: 180,
})

const filteredDevices = computed(() => {
  const keyword = deviceQuery.value.trim().toLowerCase()
  if (!keyword) return devices.value
  return devices.value.filter(item => [item.alias, item.device_id, item.dept].filter(Boolean).some(part => String(part).toLowerCase().includes(keyword)))
})

const deptOptions = computed(() => Array.from(new Set(devices.value.map(item => item.dept).filter(Boolean))).sort())
const filteredDeptOptions = computed(() => {
  const keyword = deptQuery.value.trim().toLowerCase()
  if (!keyword) return deptOptions.value
  return deptOptions.value.filter(dept => String(dept).toLowerCase().includes(keyword))
})
const secondaryItems = computed(() => {
  if (stage.value === 'compose') {
    return [
      { key: 'info', label: '通知', active: level.value === 'info', action: () => { level.value = 'info' } },
      { key: 'notice', label: '注意', active: level.value === 'notice', action: () => { level.value = 'notice' } },
      { key: 'warn', label: '警告', active: level.value === 'warn', action: () => { level.value = 'warn' } },
      { key: 'emergency', label: '紧急', active: level.value === 'emergency', action: () => { level.value = 'emergency' } },
    ]
  }
  return [
    { key: 'device', label: '设备', active: targetKind.value === 'device', action: () => { targetKind.value = 'device' } },
    { key: 'dept', label: '部门', active: targetKind.value === 'dept', action: () => { targetKind.value = 'dept' } },
    { key: 'all', label: '全员', active: targetKind.value === 'all', action: () => { targetKind.value = 'all' } },
  ]
})

const selectedDevices = computed(() => devices.value.filter(item => selectedDeviceIds.value.includes(item.device_id)))
const targetDevices = computed(() => {
  if (targetKind.value === 'all') return devices.value
  if (targetKind.value === 'dept') return devices.value.filter(item => item.dept === selectedDept.value)
  return selectedDevices.value
})
const onlineCount = computed(() => targetDevices.value.filter(item => item.online).length)
const isReadOnly = computed(() => !auth.permissions.canSendMessages)
const canContinue = computed(() => targetDevices.value.length > 0)
const canSend = computed(() => canContinue.value && title.value.trim() && body.value.trim())
const shellTone = computed(() => loading.value || sending.value ? 'loading' : levelTone(level.value))

function toggleDevice(deviceId: string) {
  const exists = selectedDeviceIds.value.includes(deviceId)
  if (exists) {
    selectedDeviceIds.value = selectedDeviceIds.value.filter(item => item !== deviceId)
    return
  }
  selectedDeviceIds.value = [...selectedDeviceIds.value, deviceId]
}

function chooseDept(dept: string) {
  selectedDept.value = dept
}

function nextStep() {
  if (stage.value === 'target') {
    if (!canContinue.value) {
      ElMessage.warning('请先选择目标')
      return
    }
    stage.value = 'compose'
  }
}

function prevStep() {
  stage.value = 'target'
}

function buildPayload(kind: TargetKind, value: string): SendBody {
  const ack_mode = level.value === 'warn' || level.value === 'emergency' ? 'acknowledged' : 'received'
  return {
    level: level.value,
    title: title.value.trim(),
    body: body.value.trim(),
    ack_mode,
    ttl: 3600,
    target_kind: kind,
    target_value: value,
  }
}

async function send() {
  if (isReadOnly.value) {
    ElMessage.warning('当前角色仅可查看，不能发送消息')
    return
  }
  if (!canSend.value) {
    ElMessage.warning('请完成目标和内容')
    return
  }

  sending.value = true
  try {
    let firstMessageId = ''
    let sentIndividually = false

    if (targetKind.value === 'all') {
      const created = await messagesApi.send(buildPayload('all', ''))
      firstMessageId = created.msg_id
    } else if (targetKind.value === 'dept') {
      if (!selectedDept.value) throw new Error('没有目标部门')
      const created = await messagesApi.send(buildPayload('dept', selectedDept.value))
      firstMessageId = created.msg_id
    } else {
      const deviceIds = selectedDeviceIds.value
      if (!deviceIds.length) throw new Error('没有目标设备')
      sentIndividually = deviceIds.length > 1
      for (const deviceId of deviceIds) {
        const created = await messagesApi.send(buildPayload('device', deviceId))
        if (!firstMessageId) firstMessageId = created.msg_id
      }
    }

    ElMessage.success('已发送')
    if (firstMessageId && !sentIndividually) {
      router.push({ name: 'history-detail', params: { msgId: firstMessageId } })
    } else {
      router.push({ name: 'history' })
    }
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || e?.message || '发送失败')
  } finally {
    sending.value = false
  }
}
</script>

<template>
  <ConsoleShell
    active-main="send"
    :secondary-items="secondaryItems"
    :tone="shellTone"
  >
    <div v-if="stage === 'target'" class="send-stack">
      <div class="hero-head" :class="{ searchable: targetKind !== 'all' }">
        <div class="hero-title">{{ targetKind === 'device' ? '选择设备' : targetKind === 'dept' ? '选择部门' : '全员广播' }}</div>
        <input v-if="targetKind === 'device'" v-model="deviceQuery" class="search-input" placeholder="搜索设备 / 部门" />
        <input v-else-if="targetKind === 'dept'" v-model="deptQuery" class="search-input" placeholder="搜索部门" />
      </div>
      <div class="hero-line">{{ targetDevices.length }} 台　在线 {{ onlineCount }} 台</div>
      <div v-if="isReadOnly" class="hero-line readonly-hint">访客可以预览发送流程，最后发送会被拒绝。</div>

      <template v-if="targetKind === 'device'">
        <div v-if="filteredDevices.length" class="picker-list">
          <button
            v-for="item in filteredDevices"
            :key="item.device_id"
            type="button"
            class="picker-row"
            :class="{ active: selectedDeviceIds.includes(item.device_id) }"
            @click="toggleDevice(item.device_id)"
          >
            <span class="col-name">{{ item.alias || item.device_id }}</span>
            <span class="col-mid">{{ item.dept || '—' }}</span>
            <span class="col-status" :class="item.online ? 'success' : 'warning'">{{ item.online ? '在线' : '离线' }}</span>
          </button>
        </div>
        <div v-else class="empty-copy">
          <strong>没有匹配设备</strong>
          <span>换个关键词试试</span>
        </div>
      </template>

      <template v-else-if="targetKind === 'dept'">
        <div v-if="filteredDeptOptions.length" class="picker-list">
          <button
            v-for="dept in filteredDeptOptions"
            :key="dept"
            type="button"
            class="picker-row"
            :class="{ active: selectedDept === dept }"
            @click="chooseDept(dept)"
          >
            <span class="col-name">{{ dept }}</span>
            <span class="col-mid">{{ devices.filter(item => item.dept === dept).length }} 台</span>
            <span class="col-status">{{ devices.filter(item => item.dept === dept && item.online).length }} 在线</span>
          </button>
        </div>
        <div v-else class="empty-copy">
          <strong>暂无部门</strong>
          <span>{{ deptOptions.length ? '换个关键词试试' : '设备写入部门后会显示在这里' }}</span>
        </div>
      </template>

      <div v-else class="summary-box">
        <strong>所有设备都会收到这条消息</strong>
        <span>当前共 {{ targetDevices.length }} 台设备，在线 {{ onlineCount }} 台</span>
      </div>
    </div>

    <div v-else class="send-stack compose-stack">
      <div class="hero-title">{{ levelLabel(level) }}</div>
      <div class="hero-line">{{ targetDevices.length }} 台</div>
      <div class="field-block">
        <label>标题</label>
        <input v-model="title" class="plain-input" maxlength="64" placeholder="输入标题" />
      </div>
      <div class="field-block body-block">
        <label>内容</label>
        <input v-model="body" class="plain-input body-input" maxlength="480" placeholder="输入消息内容" />
      </div>
    </div>

    <template #footer>
      <div class="footer-actions">
        <button v-if="stage !== 'target'" class="text-action" @click="prevStep">返回</button>
        <button v-if="stage !== 'compose'" class="screen-btn" @click="nextStep">继续</button>
        <button v-else class="screen-btn" :disabled="!canSend || sending" @click="send">发送</button>
      </div>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.send-stack { display: grid; gap: 10px; }
.readonly-hint { color: var(--bo-console-warning); }
.picker-list { display: grid; }
.picker-row {
  display: grid;
  grid-template-columns: minmax(0, 1.3fr) 120px 84px;
  gap: 12px;
  padding: 10px 0;
  border: none;
  border-top: 2px solid var(--bo-console-line);
  background: none;
  text-align: left;
  font: inherit;
  cursor: pointer;
}
.static-row { cursor: default; }
.summary-box, .field-block { display: grid; gap: 6px; }
.summary-box strong { font-size: 22px; }
.summary-box span { color: var(--bo-console-text-soft); }
.body-input { line-height: 1.4; }
.body-block { margin-top: 6px; }
@container beacon-shell (max-width: 900px) {
  .picker-row { grid-template-columns: minmax(0, 1fr); gap: 4px; }
}
</style>
