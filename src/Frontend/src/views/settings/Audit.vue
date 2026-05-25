<script setup lang="ts">
import { ref } from 'vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { auditApi } from '@/api'
import { fmtDateTime } from '@/lib/console'
import type { AuditEntry } from '@/api/types'

const entries = ref<AuditEntry[]>([])
const openedId = ref<number | null>(null)

const { loading } = useAsyncLoader(async () => {
  const response = await auditApi.list({ limit: 120 })
  entries.value = response.audit
}, {
  streamEvents: ['audit.created'],
  streamDebounceMs: 180,
})

function targetText(item: AuditEntry): string {
  if (item.target_value) return `${item.target_kind}:${item.target_value}`
  return item.target_kind || '—'
}

function toggleDetail(id: number) {
  openedId.value = openedId.value === id ? null : id
}
</script>

<template>
  <div class="settings-stack">
    <div class="hero-title">审计</div>

    <div v-if="entries.length" class="audit-list">
      <button
        v-for="item in entries"
        :key="item.id"
        type="button"
        class="audit-row"
        :class="{ open: openedId === item.id }"
        @click="toggleDetail(item.id)"
      >
        <span class="col-name">{{ item.actor || 'system' }}</span>
        <span class="col-mid">{{ item.action }}</span>
        <span class="col-target">{{ targetText(item) }}</span>
        <span v-if="openedId === item.id" class="audit-detail">
          <span>时间 {{ fmtDateTime(item.ts) }}</span>
          <span>来源 {{ item.actor_type || '—' }} · IP {{ item.ip || '—' }}</span>
          <span v-if="item.detail_json">详情 {{ item.detail_json }}</span>
        </span>
      </button>
    </div>
    <div v-else class="empty-copy">
      <strong>{{ loading ? '加载中…' : '暂无审计记录' }}</strong>
      <span>后端返回后会显示在这里</span>
    </div>
  </div>
</template>

<style scoped>
.settings-stack { display: grid; gap: 8px; }
.audit-list { display: grid; }
.audit-row {
  display: grid;
  grid-template-columns: minmax(0, 1.1fr) minmax(0, 1fr) minmax(0, 1.3fr);
  gap: 10px;
  padding: 10px 0;
  border: none;
  border-top: 2px solid var(--bo-console-line);
  background: none;
  color: inherit;
  text-align: left;
  font: inherit;
  cursor: pointer;
}
.audit-row.open { row-gap: 6px; }
.audit-detail {
  grid-column: 1 / -1;
  display: grid;
  gap: 2px;
  min-width: 0;
  color: var(--bo-console-text-muted);
  font-size: 14px;
  overflow-wrap: anywhere;
}
@container beacon-shell (max-width: 900px) {
  .audit-row { grid-template-columns: minmax(0, 1fr); gap: 4px; }
}
</style>
