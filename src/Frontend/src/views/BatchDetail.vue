<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { batchesApi, devicesApi } from '@/api'
import { batchStatusLabel, batchTone, deviceLabel, deviceStatusLabel, fmtDate, fmtDateTime } from '@/lib/console'
import { useAuth } from '@/stores/auth'
import type { Batch, Device } from '@/api/types'

const route = useRoute()
const router = useRouter()
const auth = useAuth()

const batchUuid = computed(() => route.params.batchUuid as string)
const canMutate = computed(() => auth.permissions.canMutate)
const batch = ref<Batch | null>(null)
const devices = ref<Device[]>([])
const rotating = ref(false)
const deleting = ref(false)
const restoring = ref(false)
const revokeMode = ref(false)
const revokeReason = ref('')
const latestSecret = ref('')
const isRevoked = computed(() => !!batch.value?.revoked)

const { loading, reload: load } = useAsyncLoader(async () => {
  const info = await batchesApi.get(batchUuid.value)
  batch.value = info
  const deviceResp = await devicesApi.list({ batch: batchUuid.value })
  devices.value = deviceResp.devices
}, {
  watchSources: batchUuid,
  streamEvents: ['batch.updated', 'batch.revoked', 'batch.restored', 'batch.deleted', 'device.online', 'device.offline', 'device.updated'],
  streamFilter: event => {
    const eventBatchUuid = String(event.batch_uuid || '')
    if (String(event.type || '') === 'batch.deleted' && eventBatchUuid === batchUuid.value) {
      void router.push({ name: 'batches' })
      return false
    }
    return eventBatchUuid === batchUuid.value
  },
  streamDebounceMs: 180,
})

async function exportCsv() {
  if (!batch.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能导出批次密钥')
    return
  }
  const full = await batchesApi.get(batch.value.batch_uuid)
  const csv = `batch_uuid,batch_secret\n${full.batch_uuid},${full.batch_secret || ''}\n`
  const blob = new Blob([csv], { type: 'text/csv;charset=utf-8' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = `batch-${full.batch_uuid}.csv`
  a.click()
  URL.revokeObjectURL(a.href)
}

async function rotateSecret() {
  if (!batch.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能轮换密钥')
    return
  }
  rotating.value = true
  try {
    const result = await batchesApi.rotateSecret(batch.value.batch_uuid)
    latestSecret.value = result.batch_secret
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '轮换失败')
  } finally {
    rotating.value = false
  }
}

async function revokeBatch() {
  if (!batch.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能撤销批次')
    return
  }
  try {
    await batchesApi.revoke(batch.value.batch_uuid, revokeReason.value)
    revokeMode.value = false
    revokeReason.value = ''
    ElMessage.success('批次已撤销')
    await load()
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '撤销失败')
  }
}

async function deleteBatch() {
  if (!batch.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能删除批次')
    return
  }
  deleting.value = true
  try {
    await batchesApi.deleteBatch(batch.value.batch_uuid)
    ElMessage.success('批次已删除')
    await router.push({ name: 'batches' })
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '删除失败')
  } finally {
    deleting.value = false
  }
}

async function restoreBatch() {
  if (!batch.value) return
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能恢复批次')
    return
  }
  restoring.value = true
  try {
    await batchesApi.restoreBatch(batch.value.batch_uuid)
    ElMessage.success('批次已恢复')
    await load()
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '恢复失败')
  } finally {
    restoring.value = false
  }
}
</script>

<template>
  <ConsoleShell
    active-main="batches"
    :tone="loading ? 'loading' : batch ? batchTone(batch) : 'normal'"
    :title="batchUuid"
  >
    <div v-if="batch" class="detail-stack">
      <div class="hero-status" :class="batchTone(batch)">{{ batchStatusLabel(batch) }}</div>
      <div class="meta-line">计划 {{ batch.produced_count || 0 }} 台　实际上线 {{ batch.online_count ?? 0 }} 台</div>
      <div class="meta-line">创建于 {{ fmtDateTime(batch.created_at) }}</div>

      <div class="divider" />
      <div class="label">标识</div>
      <div class="field-row"><span>别名</span><b>{{ batch.alias || '—' }}</b></div>

      <div class="divider" />
      <div class="label">操作</div>
      <template v-if="canMutate">
        <div class="action-copy">导出当前批次的设备清单。</div>
        <button class="action-btn" @click="exportCsv">导出 CSV</button>

        <template v-if="!isRevoked">
          <div class="action-copy">为该批次重新生成密钥（旧密钥立即失效）。</div>
          <button class="action-btn" :disabled="rotating" @click="rotateSecret">轮换密钥</button>
          <div v-if="latestSecret" class="secret-box">新密钥：{{ latestSecret }}</div>

          <div class="action-copy">撤销该批次（设备将无法再上报数据）。</div>
          <button class="action-btn danger" :disabled="!!batch.revoked" @click="revokeMode = true">撤销批次</button>
          <div v-if="revokeMode" class="revoke-box">
            <textarea v-model="revokeReason" class="plain-textarea" rows="2" placeholder="撤销原因（可选）" />
            <div class="inline-actions">
              <button class="action-btn danger" @click="revokeBatch">确认撤销</button>
              <button class="action-btn" @click="revokeMode = false">取消</button>
            </div>
          </div>
        </template>

        <template v-else>
          <div class="action-copy">恢复该批次（设备可重新接入，状态变回有效）。</div>
          <button class="action-btn" :disabled="restoring" @click="restoreBatch">恢复批次</button>

          <div class="action-copy">删除该批次（直接物理删除批次和关联设备记录，无法恢复）。</div>
          <button class="action-btn danger" :disabled="deleting" @click="deleteBatch">删除批次</button>
        </template>
      </template>
      <div v-else class="action-copy">当前角色仅可查看批次详情，不能导出密钥、轮换、撤销、恢复或删除批次。</div>

      <div class="divider" />
      <div class="label">设备</div>
      <div v-if="devices.length" class="console-list device-list">
        <button
          v-for="item in devices"
          :key="item.device_id"
          type="button"
          class="console-list-row batch-device-row"
          @click="router.push({ name: 'device-detail', params: { deviceId: item.device_id } })"
        >
          <span class="col-name">{{ deviceLabel(item) }}</span>
          <span class="col-mid">{{ item.dept || '—' }}</span>
          <span class="col-status" :class="item.online ? 'success' : 'warning'">{{ deviceStatusLabel(item) }}</span>
        </button>
      </div>
      <div v-else class="empty-copy">
        <strong>还没有设备</strong>
        <span>该批次下暂时没有上线设备</span>
      </div>

      <div class="tail-copy">{{ batch.batch_uuid }} · 创建于 {{ fmtDate(batch.created_at) }}</div>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'batches' })">返回</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.detail-stack {
  display: grid;
  gap: 8px;
}
.hero-status { font-size: 24px; font-weight: 700; }
.meta-line, .tail-copy { color: var(--bo-console-text-soft); font-size: 15px; }
.field-row {
  --bo-console-field-columns: 72px 1fr;
  --bo-console-field-font: var(--bo-console-font-control);
  --bo-console-field-label-font: var(--bo-console-font-control);
  --bo-console-field-label-font-mobile: var(--bo-console-font-body);
}
.action-copy { font-size: 15px; color: var(--bo-console-text-soft); }
.secret-box { padding: 6px 0; font-family: var(--bo-font-mono); word-break: break-all; }
.revoke-box { display: grid; gap: 8px; --bo-console-input-font: 16px; }
.inline-actions { display: flex; gap: 16px; }
.batch-device-row { --bo-console-row-columns: minmax(0, 1.2fr) 120px 96px; }
</style>
