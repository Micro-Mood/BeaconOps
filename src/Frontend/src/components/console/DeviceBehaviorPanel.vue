<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref, watch } from 'vue'
import VChart from 'vue-echarts'
import { BarChart } from 'echarts/charts'
import { GridComponent, TooltipComponent } from 'echarts/components'
import { use } from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { devicesApi } from '@/api'
import { useAsyncLoader } from '@/composables/useAsyncLoader'
import { useStream } from '@/stores/stream'
import type { DeviceBehaviorPoint, DeviceBehaviorTimeline } from '@/api/types'

use([CanvasRenderer, BarChart, GridComponent, TooltipComponent])

const WINDOW_OPTIONS = [1, 3, 6, 12, 24] as const
type WindowHours = (typeof WINDOW_OPTIONS)[number]

const BUCKET_BY_WINDOW: Record<WindowHours, number> = {
  1: 60,
  3: 180,
  6: 300,
  12: 600,
  24: 1200,
}

const SERIES = [
  { key: 'static_s', label: '静止', color: '#7b8794' },
  { key: 'walk_slow_s', label: '低幅活动', color: '#2b7a78' },
  { key: 'walk_fast_s', label: '中幅活动', color: '#2f855a' },
  { key: 'run_s', label: '高幅活动', color: '#d97706' },
  { key: 'shake_or_fall_s', label: '剧烈波动', color: '#c2410c' },
] as const

const props = defineProps<{
  deviceId: string
}>()

const stream = useStream()
const windowHours = ref<WindowHours>(6)
const liveNow = ref(nowSec())
const viewEnd = ref(liveNow.value)
const timeline = ref<DeviceBehaviorTimeline | null>(null)

const windowSeconds = computed(() => windowHours.value * 3600)
const bucketSeconds = computed(() => BUCKET_BY_WINDOW[windowHours.value])
const todayStart = computed(() => localDayStartSec(liveNow.value))
const minViewEnd = computed(() => Math.min(liveNow.value, todayStart.value + windowSeconds.value))
const visibleUntil = computed(() => clamp(viewEnd.value, minViewEnd.value, liveNow.value))
const visibleSince = computed(() => Math.max(todayStart.value, visibleUntil.value - windowSeconds.value))
const isLiveWindow = computed(() => liveNow.value - visibleUntil.value <= bucketSeconds.value)
const hasData = computed(() => (timeline.value?.points || []).some(point => pointTotal(point) > 0))
const canScrollLeft = computed(() => visibleSince.value > todayStart.value)
const canScrollRight = computed(() => visibleUntil.value < liveNow.value)
const chartOption = computed(() => {
  const points = timeline.value?.points || []
  const axisInterval = Math.max(0, Math.ceil(points.length / 6) - 1)

  return {
    animation: false,
    grid: {
      left: 28,
      right: 2,
      top: 4,
      bottom: 18,
    },
    tooltip: {
      trigger: 'axis',
      axisPointer: { type: 'shadow' },
      backgroundColor: 'rgba(255, 255, 255, 0.96)',
      borderColor: 'rgba(24, 58, 66, 0.14)',
      borderWidth: 1,
      padding: [8, 10],
      textStyle: { color: '#202124', fontSize: 12 },
      extraCssText: [
        'border-radius: 10px',
        'box-shadow: 0 8px 18px rgba(10, 24, 29, 0.12)',
        'backdrop-filter: blur(6px)',
      ].join(';'),
      formatter(params: any) {
        const items = Array.isArray(params) ? params : [params]
        const title = items[0]?.axisValueLabel || ''
        const rows = items
          .filter((item: any) => Number(item.value || 0) > 0)
          .map((item: any) => {
            const minutes = Math.round(Number(item.value || 0) / 60)
            const color = String(item.color || '#183a42')
            return [
              '<div style="display:flex;align-items:center;justify-content:space-between;gap:14px;min-width:124px;">',
              `<span style="display:inline-flex;align-items:center;gap:6px;color:rgba(32,33,36,0.78);"><i style="width:7px;height:7px;border-radius:999px;background:${color};display:inline-block;"></i>${item.seriesName}</span>`,
              `<strong style="font-weight:700;color:#202124;">${minutes}m</strong>`,
              '</div>',
            ].join('')
          })
        if (!rows.length) {
          rows.push('<div style="color:rgba(32,33,36,0.62);">这一分钟没有活动记录</div>')
        }
        return [
          `<div style="font-size:11px;font-weight:700;color:rgba(32,33,36,0.58);margin-bottom:6px;letter-spacing:0.02em;">${title}</div>`,
          ...rows,
        ].join('')
      },
    },
    xAxis: {
      type: 'category',
      data: points.map(point => formatClock(point.ts)),
      axisTick: { show: false },
      axisLine: { lineStyle: { color: 'rgba(32, 33, 36, 0.22)' } },
      axisLabel: {
        color: 'rgba(32, 33, 36, 0.72)',
        fontSize: 10,
        interval: axisInterval,
      },
    },
    yAxis: {
      type: 'value',
      min: 0,
      max: bucketSeconds.value,
      splitNumber: 3,
      axisTick: { show: false },
      axisLine: { show: false },
      splitLine: { lineStyle: { color: 'rgba(32, 33, 36, 0.1)' } },
      axisLabel: {
        color: 'rgba(32, 33, 36, 0.58)',
        fontSize: 10,
        formatter(value: number) {
          return `${Math.round(value / 60)}m`
        },
      },
    },
    series: SERIES.map(series => ({
      name: series.label,
      type: 'bar',
      stack: 'behavior',
      barGap: '0%',
      emphasis: { focus: 'series' },
      itemStyle: { color: series.color },
      data: points.map(point => Number(point[series.key] || 0)),
    })),
  }
})

const { loading, error, reload } = useAsyncLoader(async () => {
  timeline.value = await devicesApi.behavior(props.deviceId, {
    since: visibleSince.value,
    until: visibleUntil.value,
    bucket_s: bucketSeconds.value,
  })
}, {
  watchSources: computed(() => `${props.deviceId}|${visibleSince.value}|${visibleUntil.value}|${bucketSeconds.value}`),
})

watch(windowHours, () => {
  liveNow.value = nowSec()
  viewEnd.value = liveNow.value
})

watch(() => props.deviceId, () => {
  liveNow.value = nowSec()
  viewEnd.value = liveNow.value
  timeline.value = null
})

watch(() => stream.connected, connected => {
  if (!connected) return
  const wasLive = isLiveWindow.value
  liveNow.value = nowSec()
  if (wasLive) viewEnd.value = liveNow.value
  void reload()
})

let stop = () => {}

onMounted(() => {
  stop = stream.on('device.behavior', (event: any) => {
    if (String(event?.device_id || '') !== props.deviceId) return
    const wasLive = isLiveWindow.value
    const nextNow = Number(event?.ts || 0) || nowSec()
    liveNow.value = Math.max(liveNow.value, nextNow)
    if (!wasLive) return
    viewEnd.value = liveNow.value
    void reload()
  })
})

onUnmounted(() => {
  stop()
})

function setWindow(hours: WindowHours) {
  if (windowHours.value === hours) return
  windowHours.value = hours
}

function shiftWindow(direction: -1 | 1) {
  const nextEnd = visibleUntil.value + direction * (windowSeconds.value / 2)
  viewEnd.value = clamp(nextEnd, minViewEnd.value, liveNow.value)
}

function nowSec(): number {
  return Math.floor(Date.now() / 1000)
}

function localDayStartSec(ts: number): number {
  const date = new Date(ts * 1000)
  date.setHours(0, 0, 0, 0)
  return Math.floor(date.getTime() / 1000)
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max)
}

function formatClock(ts: number): string {
  const date = new Date(ts * 1000)
  const hours = String(date.getHours()).padStart(2, '0')
  const minutes = String(date.getMinutes()).padStart(2, '0')
  return `${hours}:${minutes}`
}

function pointTotal(point: DeviceBehaviorPoint): number {
  return point.static_s + point.walk_slow_s + point.walk_fast_s + point.run_s + point.shake_or_fall_s
}
</script>

<template>
  <section class="behavior-inline">
    <div class="chart-shell" :class="{ loading, empty: !loading && !error && !hasData }">
      <VChart v-if="!error && hasData" class="behavior-chart" :option="chartOption" autoresize />
      <div v-else class="chart-copy">
        <strong>{{ error ? '图表加载失败' : loading ? '正在加载图表' : '暂时没有图表数据' }}</strong>
        <span>{{ error || (loading ? '正在读取当前窗口的数据。' : '设备继续上报后会显示。') }}</span>
      </div>
    </div>

    <div class="control-row">
      <button type="button" class="shift-btn" :disabled="!canScrollLeft" @click="shiftWindow(-1)">&lt;</button>
      <button
        v-for="hours in WINDOW_OPTIONS"
        :key="hours"
        type="button"
        class="window-btn"
        :class="{ active: windowHours === hours, live: windowHours === hours && isLiveWindow }"
        @click="setWindow(hours)"
      >
        {{ hours }}H
      </button>
      <button type="button" class="shift-btn" :disabled="!canScrollRight" @click="shiftWindow(1)">&gt;</button>
    </div>
  </section>
</template>

<style scoped>
.behavior-inline {
  display: grid;
  gap: 4px;
  min-width: 0;
}

.chart-shell {
  min-height: 116px;
  overflow: hidden;
}

.behavior-chart {
  width: 100%;
  height: 116px;
}

.chart-copy {
  height: 116px;
  display: grid;
  place-content: center;
  gap: 4px;
  padding: 4px 0;
  text-align: left;
}

.chart-copy strong {
  font-size: 14px;
  line-height: 1.2;
}

.chart-copy span {
  color: var(--bo-console-text-muted);
  font-size: 12px;
  line-height: 1.45;
}

.control-row {
  display: grid;
  grid-template-columns: 26px repeat(5, minmax(0, auto)) 26px;
  justify-content: start;
  gap: 8px;
}

.shift-btn,
.window-btn {
  min-width: 0;
  height: 24px;
  border: none;
  border-bottom: 2px solid transparent;
  background: none;
  color: var(--bo-console-text-soft);
  font: inherit;
  font-size: 12px;
  font-weight: 700;
  cursor: pointer;
  transition: border-color 120ms ease, color 120ms ease;
}

.window-btn.active {
  border-bottom-color: rgba(24, 58, 66, 0.45);
  color: var(--bo-console-text);
}

.window-btn.active.live {
  border-bottom-color: var(--bo-console-shell);
}

.shift-btn:disabled,
.window-btn:disabled {
  cursor: default;
  opacity: 0.45;
}

@media (max-width: 900px) {
  .control-row {
    gap: 6px;
  }
}
</style>