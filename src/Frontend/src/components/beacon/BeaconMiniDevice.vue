<script setup lang="ts">
import { computed } from 'vue'
import BeaconDeviceFrame from './BeaconDeviceFrame.vue'

const props = withDefaults(defineProps<{
  deviceId: string
  alias?: string | null
  dept?: string | null
  status?: string
  title?: string
  body?: string
  level?: string
}>(), {
  alias: '',
  dept: '',
  status: 'queued',
  title: '',
  body: '',
  level: 'info',
})

const labels: Record<string, string> = {
  queued: '待发送',
  sent: '已发送',
  delivered: '已收到',
  displayed: '已显示',
  acknowledged: '已确认',
  expired: '已过期',
  failed: '失败',
  offline: '离线',
}

const stateLabel = computed(() => labels[props.status || 'queued'] || props.status || '未知')
const onlineLike = computed(() => !['queued', 'offline', 'failed', 'expired'].includes(props.status || ''))
const name = computed(() => props.alias || props.deviceId)
const meta = computed(() => [props.dept, props.deviceId].filter(Boolean).join(' · '))
</script>

<template>
  <article class="mini-card" :class="[`status-${props.status || 'queued'}`]">
    <BeaconDeviceFrame variant="mini" :level="props.level" :state="props.status" :online="onlineLike">
      <div class="mini-screen">
        <div class="mini-topline">
          <span class="mini-dot" />
          <span>{{ stateLabel }}</span>
        </div>
        <strong>{{ props.title || '(无标题)' }}</strong>
        <p>{{ props.body || '没有正文内容' }}</p>
      </div>
    </BeaconDeviceFrame>
    <div class="mini-meta">
      <b>{{ name }}</b>
      <span>{{ meta }}</span>
    </div>
  </article>
</template>

<style scoped>
.mini-card {
  min-width: 0;
  padding: 10px;
  border: 1px solid var(--bo-border);
  border-radius: 10px;
  background: rgba(255, 255, 255, 0.78);
}

.mini-screen {
  display: grid;
  align-content: start;
  gap: 3px;
  padding: 6px 8px;
  color: #dff7ff;
  width: 100%;
  height: 100%;
  box-sizing: border-box;
  overflow: hidden;
}

.mini-topline {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  color: #9bdcfb;
  font-size: 9px;
  font-weight: 700;
  line-height: 1;
}

.mini-dot {
  width: 5px;
  height: 5px;
  border-radius: 50%;
  background: currentColor;
  box-shadow: 0 0 8px currentColor;
}

.mini-screen strong {
  min-width: 0;
  color: #ffffff;
  font-size: 11px;
  line-height: 1.2;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.mini-screen p {
  margin: 0;
  color: rgba(223, 247, 255, .82);
  font-size: 9px;
  line-height: 1.3;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
}

.mini-meta {
  display: grid;
  gap: 2px;
  margin-top: 8px;
  min-width: 0;
}

.mini-meta b,
.mini-meta span {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.mini-meta b {
  color: var(--bo-text);
  font-size: 13px;
}

.mini-meta span {
  color: #64748b;
  font-size: 11px;
}

.status-acknowledged {
  border-color: rgba(22, 163, 74, .32);
}
.status-displayed,
.status-delivered,
.status-sent {
  border-color: rgba(26, 115, 232, .28);
}
.status-failed {
  border-color: rgba(220, 38, 38, .32);
}
.status-expired,
.status-queued,
.status-offline {
  border-color: rgba(148, 163, 184, .35);
}
</style>
