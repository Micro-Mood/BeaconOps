<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { messagesApi } from '@/api'
import { fmtDateTime, levelLabel, levelTone, messageStatusLabel, progressLabel, recipientStatusLabel, recipientTone, targetLabel } from '@/lib/console'
import type { Message, Recipient } from '@/api/types'

const route = useRoute()
const router = useRouter()

const msgId = computed(() => route.params.msgId as string)
const message = ref<Message | null>(null)
const recipients = ref<Recipient[]>([])

const { loading } = useAsyncLoader(async () => {
  const [msgResp, recipientResp] = await Promise.all([
    messagesApi.get(msgId.value),
    messagesApi.recipients(msgId.value),
  ])
  message.value = msgResp
  recipients.value = recipientResp.recipients
}, {
  watchSources: msgId,
  streamEvents: ['message.status_change', 'message.recipient_update', 'message.event'],
  streamFilter: event => event.msg_id === msgId.value,
  streamDebounceMs: 120,
})

const sortedRecipients = computed(() => [...recipients.value].sort((a, b) => (a.dept || '').localeCompare(b.dept || '') || (a.alias || a.device_id).localeCompare(b.alias || b.device_id)))

function recipientTail(row: Recipient): string {
  if (row.status === 'failed' && row.attempt_count) return `尝试${row.attempt_count}次`
  if (row.status === 'offline') return '离线'
  return recipientStatusLabel(row.status)
}
</script>

<template>
  <ConsoleShell
    active-main="history"
    :tone="loading ? 'loading' : message ? levelTone(message.level) : 'normal'"
    :title="message ? levelLabel(message.level) : '通知'"
  >
    <div v-if="message" class="detail-stack">
      <div class="hero-title">{{ message.title || '未命名消息' }}</div>
      <div class="hero-body">{{ message.body || '这里是消息内容…' }}</div>
      <div class="hero-line">{{ progressLabel(message) }}</div>
      <div class="hero-line">{{ fmtDateTime(message.created_at) }}</div>
      <div class="hero-line status-line" :class="message ? levelTone(message.level) : 'normal'">
        {{ messageStatusLabel(message.status) }} · {{ targetLabel(message) }}
      </div>

      <div class="divider" />
      <div class="label">目标设备</div>

      <div v-if="sortedRecipients.length" class="console-list recipient-list">
        <div v-for="row in sortedRecipients" :key="row.device_id" class="console-list-row static-row recipient-row">
          <span class="col-name">{{ row.alias || row.device_id }}</span>
          <span class="col-mid">{{ row.dept || '—' }}</span>
          <span class="col-status" :class="recipientTone(row.status)">{{ recipientTail(row) }}</span>
        </div>
      </div>
      <div v-else class="empty-copy">
        <strong>暂无目标设备</strong>
        <span>后端还没有返回收件人记录</span>
      </div>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'history' })">返回</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.detail-stack { display: grid; gap: 8px; }
.hero-body { font-size: 18px; line-height: 1.45; white-space: pre-wrap; }
.divider { --bo-console-divider-margin: 10px 0 2px; }
.recipient-row { --bo-console-row-columns: minmax(0, 1.2fr) 120px 110px; }
@container beacon-shell (max-width: 900px) {
  .hero-body { font-size: 15px; }
}
</style>
