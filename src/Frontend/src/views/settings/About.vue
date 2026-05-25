<script setup lang="ts">
import { ref } from 'vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { useStream } from '@/stores/stream'
import { fmtDateTime } from '@/lib/console'

const stream = useStream()
const version = ref('—')
const time = ref('—')
const envName = ref('—')

useAsyncLoader(async () => {
  try {
    const response = await fetch('/beacon/health', { credentials: 'same-origin' })
    if (!response.ok) throw new Error('health unavailable')
    const data = await response.json()
    version.value = data?.version || '—'
    time.value = typeof data?.time === 'number' ? fmtDateTime(data.time) : '—'
    envName.value = data?.environment || '—'
  } catch {
    version.value = '不可用'
    time.value = '—'
    envName.value = '—'
  }
})
</script>

<template>
  <div class="settings-stack">
    <div class="hero-title">关于</div>

    <div class="info-list">
      <div class="info-row">
        <span>服务</span>
        <strong>Beacon Console</strong>
      </div>
      <div class="info-row">
        <span>实时连接</span>
        <strong>{{ stream.connected ? '已连接' : '未连接' }}</strong>
      </div>
      <div class="info-row">
        <span>版本</span>
        <strong>{{ version }}</strong>
      </div>
      <div class="info-row">
        <span>环境</span>
        <strong>{{ envName }}</strong>
      </div>
      <div class="info-row">
        <span>时间</span>
        <strong>{{ time }}</strong>
      </div>
    </div>
  </div>
</template>

<style scoped>
.settings-stack { display: grid; gap: 8px; }
.info-list { display: grid; }
.info-row { display: grid; grid-template-columns: 120px minmax(0, 1fr); gap: 12px; padding: 12px 0; border-top: 2px solid var(--bo-console-line); }
.info-row span, .info-row strong { font-size: var(--bo-console-font-control); }
@container beacon-shell (max-width: 900px) {
  .info-row { grid-template-columns: minmax(0, 1fr); gap: 4px; }
  .info-row span, .info-row strong { font-size: var(--bo-console-font-body); }
}
</style>
