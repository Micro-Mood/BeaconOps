import { defineStore } from 'pinia'
import { API_BASE } from '@/api/client'

// 全局 SSE 总线。所有页面订阅这里的 listeners(type 通配 '*' 也支持)。
// 后端事件 schema 见 data-model-v1.md §3:
//   { type: 'message.status_change', msg_id, status, ... }
//   { type: 'message.recipient_update', msg_id, device_id, ... }
//   { type: 'message.event', msg_id, device_id, event_type, ... }
//   { type: 'device.online' | 'device.offline' | 'device.health', device_id, ... }
//   { type: 'batch.revoked', batch_uuid, ... }

type Listener = (e: any) => void

interface State {
  connected: boolean
  lastError: string
  reconnectDelay: number
}

let es: EventSource | null = null
const listeners: Record<string, Set<Listener>> = {}

export const useStream = defineStore('stream', {
  state: (): State => ({ connected: false, lastError: '', reconnectDelay: 1000 }),
  actions: {
    start() {
      if (es) return
      this._connect()
    },
    stop() {
      if (es) { es.close(); es = null }
      this.connected = false
    },
    on(type: string, fn: Listener): () => void {
      const set = listeners[type] ?? (listeners[type] = new Set())
      set.add(fn)
      return () => { set.delete(fn) }
    },
    _connect() {
      try {
        es = new EventSource(`${API_BASE}/stream`, { withCredentials: true })
      } catch (e: any) {
        this.lastError = String(e?.message || e)
        this._scheduleReconnect()
        return
      }
      es.addEventListener('hello', () => {
        this.connected = true
        this.reconnectDelay = 1000
      })
      es.onmessage = (m) => {
        try {
          const obj = JSON.parse(m.data)
          const t = obj.type || ''
          if (t && listeners[t]) listeners[t].forEach(fn => { try { fn(obj) } catch {} })
          if (listeners['*'])  listeners['*'].forEach(fn => { try { fn(obj) } catch {} })
        } catch {}
      }
      es.onerror = () => {
        this.connected = false
        this.lastError = 'sse disconnected'
        if (es) { es.close(); es = null }
        this._scheduleReconnect()
      }
    },
    _scheduleReconnect() {
      const d = this.reconnectDelay
      this.reconnectDelay = Math.min(d * 2, 30000)
      setTimeout(() => { if (!es) this._connect() }, d)
    },
  },
})
