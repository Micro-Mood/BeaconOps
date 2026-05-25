<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { batchesApi } from '@/api'
import { batchStatusLabel, batchTone } from '@/lib/console'
import { useAuth } from '@/stores/auth'
import type { Batch } from '@/api/types'

const route = useRoute()
const router = useRouter()
const auth = useAuth()

const batches = ref<Batch[]>([])
const createMode = ref(false)
const createdSecret = ref('')
const form = ref({ batch_uuid: '', alias: '', produced_count: 0 })

const currentTab = computed(() => String(route.query.status || 'all'))
const canMutate = computed(() => auth.permissions.canMutate)

const { loading, reload: load } = useAsyncLoader(async () => {
  batches.value = (await batchesApi.list()).batches
}, {
  streamEvents: ['batch.created', 'batch.updated', 'batch.revoked', 'batch.restored', 'batch.deleted', 'device.online', 'device.offline'],
  streamDebounceMs: 180,
})

const secondaryItems = computed(() => [
  { key: 'all', label: '全部', to: { name: 'batches', query: { status: 'all' } }, active: currentTab.value === 'all' },
  { key: 'valid', label: '有效', to: { name: 'batches', query: { status: 'valid' } }, active: currentTab.value === 'valid' },
  { key: 'revoked', label: '已撤销', to: { name: 'batches', query: { status: 'revoked' } }, active: currentTab.value === 'revoked' },
])

const filtered = computed(() => {
  if (currentTab.value === 'valid') return batches.value.filter(item => !item.revoked)
  if (currentTab.value === 'revoked') return batches.value.filter(item => !!item.revoked)
  return batches.value
})

async function doCreate() {
  if (!canMutate.value) {
    ElMessage.warning('当前角色仅可查看，不能创建批次')
    return
  }
  if (!form.value.batch_uuid.trim()) {
    ElMessage.warning('请输入批次编号')
    return
  }
  try {
    const result = await batchesApi.create(form.value)
    createdSecret.value = result.batch_secret
    form.value = { batch_uuid: '', alias: '', produced_count: 0 }
    await load()
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || '创建失败')
  }
}
</script>

<template>
  <ConsoleShell
    active-main="batches"
    :secondary-items="secondaryItems"
    :tone="loading ? 'loading' : 'normal'"
    title="批次"
  >
    <template v-if="createMode">
      <div class="label">新批次</div>
      <div class="field-row">
        <span>编号</span>
        <input v-model="form.batch_uuid" class="plain-input" placeholder="b2026q2-0001" />
      </div>
      <div class="field-row">
        <span>别名</span>
        <input v-model="form.alias" class="plain-input" placeholder="tests" />
      </div>
      <div class="field-row">
        <span>计划</span>
        <input v-model.number="form.produced_count" class="plain-input" type="number" min="0" />
      </div>
      <div v-if="createdSecret" class="secret-box">新密钥：{{ createdSecret }}</div>
    </template>

    <template v-else-if="filtered.length">
      <div class="console-list batch-list">
        <button
          v-for="item in filtered"
          :key="item.batch_uuid"
          type="button"
          class="console-list-row batch-row"
          @click="router.push({ name: 'batch-detail', params: { batchUuid: item.batch_uuid } })"
        >
          <span class="row-main">{{ item.batch_uuid }}<small v-if="item.alias">{{ item.alias }}</small></span>
          <span class="row-mid" :class="batchTone(item)">{{ batchStatusLabel(item) }}</span>
          <span class="row-side">{{ item.online_count ?? 0 }} / {{ item.produced_count || 0 }}</span>
        </button>
      </div>
    </template>

    <template v-else>
      <div class="empty-copy">
        <strong>还没有批次</strong>
        <span>批次是设备的“出生证”</span>
      </div>
    </template>

    <template #footer>
      <div v-if="canMutate" class="footer-actions">
        <button v-if="createMode" class="footer-link" @click="createMode = false">取消</button>
        <button v-if="createMode" class="screen-btn" @click="doCreate">创建</button>
        <button v-else class="screen-btn" @click="createMode = true">+ 新批次</button>
      </div>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.batch-row { --bo-console-row-columns: minmax(0, 1.4fr) 96px 90px; }
.row-main { display: grid; gap: 2px; font-size: 18px; }
.row-main small { color: var(--bo-console-text-muted); font-size: 14px; }
.row-mid, .row-side { font-size: 18px; }
.empty-copy { --bo-console-empty-margin-top: 18px; }
.label { --bo-console-label-margin-bottom: 6px; }
.field-row {
  --bo-console-field-padding: 6px 0;
  --bo-console-field-label-font: var(--bo-console-font-control);
  --bo-console-field-label-font-mobile: var(--bo-console-font-body);
}
.secret-box { margin-top: 10px; font-family: var(--bo-font-mono); word-break: break-all; }
.footer-actions { --bo-console-footer-gap: 14px; }
@container beacon-shell (max-width: 900px) {
  .row-main { font-size: 15px; }
}
</style>
