import { onMounted, onUnmounted, ref, watch, type WatchSource } from 'vue'
import { ElMessage } from 'element-plus'
import { useStream } from '@/stores/stream'

type StreamEvent = {
  type?: string
  [key: string]: unknown
}

interface UseAsyncLoaderOptions {
  immediate?: boolean
  watchSources?: WatchSource<unknown> | WatchSource<unknown>[]
  streamEvents?: string[]
  streamFilter?: (event: StreamEvent) => boolean
  streamDebounceMs?: number
}

export function useAsyncLoader(loader: () => Promise<void>, options: UseAsyncLoaderOptions = {}) {
  const loading = ref(false)
  const error = ref<string | null>(null)
  let seq = 0
  let streamTimer: number | null = null

  async function reload() {
    const current = ++seq
    loading.value = true
    error.value = null
    try {
      await loader()
      return true
    } catch (e: any) {
      const message = e?.response?.data?.detail || e?.message || '加载失败'
      error.value = message
      ElMessage.error(message)
      return false
    } finally {
      if (current === seq) loading.value = false
    }
  }

  function scheduleReload() {
    const delay = options.streamDebounceMs ?? 120
    if (delay <= 0) {
      void reload()
      return
    }
    if (streamTimer !== null) {
      window.clearTimeout(streamTimer)
    }
    streamTimer = window.setTimeout(() => {
      streamTimer = null
      void reload()
    }, delay)
  }

  if (options.immediate !== false) {
    onMounted(() => {
      void reload()
    })
  }

  if (options.watchSources) {
    watch(options.watchSources, () => {
      void reload()
    })
  }

  if (options.streamEvents?.length) {
    const stream = useStream()
    let stop = () => {}

    onMounted(() => {
      const cleaners = options.streamEvents!.map(event => stream.on(event, payload => {
        if (options.streamFilter && !options.streamFilter(payload as StreamEvent)) {
          return
        }
        scheduleReload()
      }))

      stop = () => {
        cleaners.forEach(clean => clean())
      }
    })

    onUnmounted(() => {
      if (streamTimer !== null) {
        window.clearTimeout(streamTimer)
        streamTimer = null
      }
      stop()
    })
  }

  return { loading, error, reload }
}