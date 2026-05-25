import type { Admin, Batch, Device, Message } from '@/api/types'

export type ShellTone = 'normal' | 'loading' | 'info' | 'success' | 'warning' | 'error'

export function fmtDateTime(ts: number | null | undefined): string {
  if (!ts) return '—'
  return new Date(ts * 1000).toLocaleString('zh-CN', { hour12: false })
}

export function fmtDate(ts: number | null | undefined): string {
  if (!ts) return '—'
  return new Date(ts * 1000).toLocaleDateString('zh-CN')
}

export function fmtShort(ts: number | null | undefined): string {
  if (!ts) return '—'
  return new Date(ts * 1000).toLocaleString('zh-CN', {
    hour12: false,
    month: 'numeric',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  })
}

export function messageTitle(message: Partial<Message>): string {
  return message.title || message.body || '(无标题)'
}

export function levelLabel(level: string | null | undefined): string {
  return ({ info: '通知', notice: '注意', warn: '警告', emergency: '紧急' } as Record<string, string>)[level || ''] || '通知'
}

export function levelTone(level: string | null | undefined): ShellTone {
  return ({ info: 'info', notice: 'success', warn: 'warning', emergency: 'error' } as Record<string, ShellTone>)[level || ''] || 'normal'
}

export function messageStatusLabel(status: string | null | undefined): string {
  return ({
    queued: '排队',
    sent: '已发送',
    delivered: '已送达',
    displayed: '已显示',
    acknowledged: '已确认',
    expired: '已过期',
    failed: '失败',
  } as Record<string, string>)[status || ''] || '未知'
}

export function messageTone(status: string | null | undefined): ShellTone {
  if (status === 'acknowledged' || status === 'displayed' || status === 'delivered' || status === 'sent') return 'success'
  if (status === 'failed') return 'error'
  if (status === 'expired') return 'warning'
  if (status === 'queued') return 'info'
  return 'normal'
}

export function recipientStatusLabel(status: string | null | undefined): string {
  return ({
    queued: '排队中',
    sent: '尝试发送',
    delivered: '已送达',
    displayed: '已显示',
    acknowledged: '已确认',
    failed: '失败',
    expired: '已过期',
    offline: '离线',
  } as Record<string, string>)[status || ''] || '未知'
}

export function recipientTone(status: string | null | undefined): ShellTone {
  if (status === 'acknowledged' || status === 'displayed' || status === 'delivered') return 'success'
  if (status === 'failed') return 'error'
  if (status === 'expired' || status === 'offline') return 'warning'
  if (status === 'sent' || status === 'queued') return 'info'
  return 'normal'
}

export function targetLabel(message: Partial<Message>): string {
  if (message.target_kind === 'all') return '全员'
  if (message.target_kind === 'dept') return message.target_value ? `部门 ${message.target_value}` : '部门'
  return message.target_value || '指定设备'
}

export function progressLabel(message: Partial<Message>): string {
  const total = Number(message.recipients_total || 0)
  const ack = Number(message.recipients_acknowledged || 0)
  return `${ack} / ${total} 已确认`
}

export function deviceLabel(device: Partial<Device>): string {
  return device.alias || device.device_id || '未命名设备'
}

export function deviceStatusLabel(device: Partial<Device>): string {
  return device.online ? '在线' : '离线'
}

export function deviceTone(device: Partial<Device>): ShellTone {
  if (!device.online) return 'warning'
  return 'success'
}

export function batchStatusLabel(batch: Partial<Batch>): string {
  return batch.revoked ? '已撤销' : '有效'
}

export function batchTone(batch: Partial<Batch>): ShellTone {
  return batch.revoked ? 'warning' : 'success'
}

export function adminStateLabel(admin: Partial<Admin>): string {
  return admin.enabled ? '启用' : '已禁用'
}

export function adminRoleTone(role: string | null | undefined): ShellTone {
  if (role === 'admin') return 'error'
  if (role === 'operator') return 'warning'
  if (role === 'viewer') return 'success'
  return 'normal'
}
