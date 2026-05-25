<script setup lang="ts">
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { messagesApi } from '@/api'
import { fmtDateTime, messageStatusLabel, messageTitle, messageTone, targetLabel } from '@/lib/console'
import type { Message } from '@/api/types'

const router = useRouter()

const messages = ref<Message[]>([])
const historyQuery = ref('')

const filteredMessages = computed(() => {
  const keyword = historyQuery.value.trim().toLowerCase()
  if (!keyword) return messages.value

  return messages.value.filter(item => {
    const haystack = [
      item.msg_id,
      messageTitle(item),
      targetLabel(item),
      messageStatusLabel(item.status),
    ]

    return haystack.some(part => String(part || '').toLowerCase().includes(keyword))
  })
})

const { loading } = useAsyncLoader(async () => {
  const response = await messagesApi.list({ limit: 120 })
  messages.value = response.messages
}, {
  streamEvents: ['message.created', 'message.status_change', 'message.recipient_update', 'message.event'],
  streamDebounceMs: 150,
})
</script>

<template>
  <ConsoleShell
    active-main="history"
    :tone="loading ? 'loading' : 'normal'"
  >
    <div class="hero-head searchable">
      <div class="hero-title">历史</div>
      <input
        v-model="historyQuery"
        class="search-input"
        placeholder="搜索标题 / 目标 / 状态"
      />
    </div>

    <div v-if="filteredMessages.length" class="console-list history-list">
      <button
        v-for="item in filteredMessages"
        :key="item.msg_id"
        type="button"
        class="console-list-row history-row"
        @click="router.push({ name: 'history-detail', params: { msgId: item.msg_id } })"
      >
        <span class="row-title">{{ messageTitle(item) }}</span>
        <span class="row-time">{{ fmtDateTime(item.created_at) }}</span>
        <span class="row-meta" :class="messageTone(item.status)">{{ messageStatusLabel(item.status) }}</span>
        <span class="row-target">{{ targetLabel(item) }}</span>
      </button>
    </div>
    <div v-else-if="messages.length" class="empty-copy">
      <strong>没有匹配历史</strong>
      <span>换个关键词试试</span>
    </div>
    <div v-else class="empty-copy">
      <strong>暂无历史消息</strong>
      <span>点击“发送”创建第一条消息</span>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'send' })">去发送</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.hero-head { --bo-console-head-margin-bottom: 10px; }

.history-row { --bo-console-row-columns: minmax(0, 1fr) auto; --bo-console-row-gap: 2px 14px; }
.row-title { font-size: 22px; }
.row-time, .row-target { color: var(--bo-console-text-muted); font-size: 15px; }
.row-meta { font-size: 15px; }
.empty-copy { --bo-console-empty-margin-top: 18px; }
@container beacon-shell (max-width: 900px) {
  .row-title { font-size: 17px; }
  .row-time, .row-target, .row-meta { font-size: 15px; }
}
</style>
