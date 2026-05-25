import http from './client'
import type {
  Admin, AuditEntry, Batch, Device, DeviceBehaviorTimeline,
  Message, Recipient, TargetKind,
} from './types'

// ── auth ──
export const authApi = {
  login: (username: string, password: string) =>
    http.post('/auth/login', { username, password }).then(r => r.data),
  me:     () => http.get('/auth/me').then(r => r.data),
  logout: () => http.post('/auth/logout').then(r => r.data),
}

// ── messages ──
export interface SendBody {
  target_kind: TargetKind
  target_value: string
  level: string
  title: string
  body: string
  ttl: number
  ack_mode: string
}

export const messagesApi = {
  send:       (b: SendBody)            => http.post('/messages', b).then(r => r.data),
  list:       (params: Record<string, any> = {}) =>
    http.get<{ messages: Message[]; total: number }>('/messages', { params }).then(r => r.data),
  get:        (id: string)             => http.get<Message>(`/messages/${id}`).then(r => r.data),
  recipients: (id: string)             =>
    http.get<{ recipients: Recipient[] }>(`/messages/${id}/recipients`).then(r => r.data),
}

// ── devices ──
export const devicesApi = {
  list: (params: Record<string, any> = {}) =>
    http.get<{ devices: Device[]; total: number }>('/devices', { params }).then(r => r.data),
  get: (id: string) => http.get<Device>(`/devices/${id}`).then(r => r.data),
  behavior: (id: string, params: { since: number; until: number; bucket_s: number }) =>
    http.get<DeviceBehaviorTimeline>(`/devices/${id}/behavior`, { params }).then(r => r.data),
  patch: (id: string, body: Partial<{
    alias: string; dept: string
  }>) => http.patch(`/devices/${id}`, body).then(r => r.data),
  messages: (id: string, limit = 50) =>
    http.get<{ messages: Message[] }>(`/devices/${id}/messages`, { params: { limit } })
      .then(r => r.data),
}

// ── batches ──
export const batchesApi = {
  list:   ()                      => http.get<{ batches: Batch[] }>('/batches').then(r => r.data),
  get:    (uuid: string)          => http.get<Batch>(`/batches/${uuid}`).then(r => r.data),
  create: (body: { batch_uuid: string; alias?: string; produced_count?: number; notes?: string }) =>
    http.post<{ batch_uuid: string; batch_secret: string }>('/batches', body).then(r => r.data),
  patch:  (uuid: string, body: Partial<{ alias: string; notes: string; produced_count: number }>) =>
    http.patch(`/batches/${uuid}`, body).then(r => r.data),
  revoke: (uuid: string, reason: string) =>
    http.post(`/batches/${uuid}/revoke`, { reason }).then(r => r.data),
  deleteBatch: (uuid: string) =>
    http.post(`/batches/${uuid}/delete`).then(r => r.data),
  restoreBatch: (uuid: string) =>
    http.post(`/batches/${uuid}/restore`).then(r => r.data),
  rotateSecret: (uuid: string) =>
    http.post<{ batch_uuid: string; batch_secret: string }>(`/batches/${uuid}/secret`)
      .then(r => r.data),
}

// ── admins ──
export const adminsApi = {
  list: () => http.get<{ admins: Admin[] }>('/admins').then(r => r.data),
  create: (b: { username: string; password: string; role?: string }) =>
    http.post('/admins', b).then(r => r.data),
  patch: (u: string, b: Partial<{ password: string; role: string; enabled: boolean }>) =>
    http.patch(`/admins/${u}`, b).then(r => r.data),
  delete: (u: string) => http.delete(`/admins/${u}`).then(r => r.data),
}

// ── audit ──
export const auditApi = {
  list: (params: { actor?: string; action?: string; since?: number; until?: number; limit?: number } = {}) =>
    http.get<{ audit: AuditEntry[]; total: number }>('/audit', { params }).then(r => r.data),
}
