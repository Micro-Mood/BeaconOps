// 与 Server/services/beacon/api/app schema 对齐(data-model-v1.md)
export type Level = 'info' | 'notice' | 'warn' | 'emergency'
export type AckMode = 'none' | 'received' | 'displayed' | 'acknowledged'
export type TargetKind = 'device' | 'all' | 'dept'

// 与后端 messages.status 对齐
export type MessageStatus =
  | 'queued'
  | 'sent'
  | 'delivered'
  | 'displayed'
  | 'acknowledged'
  | 'expired'
  | 'failed'

export interface Message {
  msg_id: string
  source_type: string
  operator: string
  target_kind: TargetKind
  target_value: string
  level: Level
  title: string
  body: string
  ttl: number
  ack_mode: AckMode
  status: MessageStatus
  recipients_total: number
  recipients_delivered: number
  recipients_displayed: number
  recipients_acknowledged: number
  recipients_failed: number
  attempt_count: number
  last_error: string
  created_at: number
  updated_at: number
}

export interface Recipient {
  msg_id: string
  device_id: string
  status: string
  delivered_at: number | null
  displayed_at: number | null
  acknowledged_at: number | null
  failed_at: number | null
  attempt_count: number
  last_error: string
  alias?: string | null
  dept?: string | null
  roles_json?: string | null
}

export interface Device {
  device_id: string
  batch_uuid: string
  alias: string
  dept: string
  roles_json: string
  roles: string[]
  enabled: number
  online: number
  last_seen_at: number
  battery: number | null
  rssi: number | null
  charging: number | null
  ip: string | null
  fw_version: string | null
  battery_soc: number
  last_ip: string
  uptime_s: number
  uptime: number | null
  spiffs_pending: number | null
  ack_pending: number | null
  drop_count: number | null
  created_at: number
  updated_at: number
}

export interface DeviceBehaviorPoint {
  ts: number
  static_s: number
  walk_slow_s: number
  walk_fast_s: number
  run_s: number
  shake_or_fall_s: number
}

export interface DeviceBehaviorTimeline {
  since: number
  until: number
  bucket_s: number
  points: DeviceBehaviorPoint[]
}

export interface Batch {
  batch_uuid: string
  alias: string
  batch_secret?: string
  produced_at: number
  produced_count: number
  revoked: number
  revoked_at: number | null
  revoked_reason: string
  notes: string
  created_at: number
  updated_at: number
  device_count?: number
  online_count?: number
}

export interface Admin {
  username: string
  role: 'admin' | 'operator' | 'viewer'
  enabled: number
  created_at: number
  last_login_at: number | null
}

export interface AuditEntry {
  id: number
  ts: number
  actor_type: string
  actor: string
  action: string
  target_kind: string
  target_value: string
  detail_json: string | null
  ip: string
}
