<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { devicesApi } from '@/api'
import { deviceLabel, deviceStatusLabel } from '@/lib/console'
import type { Device } from '@/api/types'

const route = useRoute()
const router = useRouter()

const devices = ref<Device[]>([])

const currentDept = computed(() => String(route.query.dept || 'all'))

const { loading } = useAsyncLoader(async () => {
  devices.value = (await devicesApi.list({})).devices
}, {
  streamEvents: ['device.online', 'device.offline', 'device.updated', 'batch.revoked', 'batch.restored', 'batch.deleted'],
  streamDebounceMs: 180,
})

const depts = computed(() => Array.from(new Set(devices.value.map(item => item.dept).filter(Boolean))).sort())
const secondaryItems = computed(() => [
  { key: 'all', label: '全部', to: { name: 'devices', query: { dept: 'all' } }, active: currentDept.value === 'all' },
  ...depts.value.map(item => ({ key: item, label: item, to: { name: 'devices', query: { dept: item } }, active: currentDept.value === item })),
])
const filtered = computed(() => currentDept.value === 'all' ? devices.value : devices.value.filter(item => item.dept === currentDept.value))
</script>

<template>
  <ConsoleShell
    active-main="devices"
    :secondary-items="secondaryItems"
    :tone="loading ? 'loading' : 'normal'"
    title="设备"
  >
    <div v-if="filtered.length" class="console-list device-list">
      <button
        v-for="item in filtered"
        :key="item.device_id"
        type="button"
        class="console-list-row device-row"
        @click="router.push({ name: 'device-detail', params: { deviceId: item.device_id } })"
      >
        <span class="col-name">{{ deviceLabel(item) }}<small>{{ item.device_id }}</small></span>
        <span class="col-mid">{{ item.dept || '—' }}</span>
        <span class="col-status" :class="item.online ? 'success' : 'warning'">{{ deviceStatusLabel(item) }}</span>
      </button>
    </div>
    <div v-else class="empty-copy">
      <strong>还没有设备</strong>
      <span>请先在“批次”里创建批次并烧录设备</span>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'batches' })">去批次</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.device-row { --bo-console-row-columns: minmax(0, 1.2fr) 120px 96px; }
.col-name { display: grid; gap: 2px; }
.col-name small { color: var(--bo-console-text-faint); font-size: 13px; font-family: var(--bo-font-mono); }
.empty-copy { --bo-console-empty-margin-top: 18px; }
</style>
