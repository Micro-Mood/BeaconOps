import { defineStore } from 'pinia'
import { authApi } from '@/api'

const localHosts = new Set(['localhost', '127.0.0.1', '::1'])
const devBypassEnabled = import.meta.env.DEV &&
  import.meta.env.VITE_BYPASS_LOGIN === 'true' &&
  localHosts.has(window.location.hostname)
const devBypassUser = {
  username: 'local-admin',
  role: 'admin' as const,
}

interface State {
  username: string
  role: '' | 'admin' | 'operator' | 'viewer'
  ready: boolean
}

export type AuthRole = State['role']

export interface AuthPermissions {
  canSendMessages: boolean
  canManageSettings: boolean
  canMutate: boolean
}

function permissionsForRole(role: AuthRole): AuthPermissions {
  return {
    canSendMessages: role === 'admin' || role === 'operator',
    canManageSettings: role === 'admin',
    canMutate: role === 'admin' || role === 'operator',
  }
}

export const useAuth = defineStore('auth', {
  state: (): State => ({ username: '', role: '', ready: false }),
  getters: {
    isLogged: (s) => !!s.username,
    isAdmin:  (s) => s.role === 'admin',
    isViewer: (s) => s.role === 'viewer',
    permissions: (s): AuthPermissions => permissionsForRole(s.role),
  },
  actions: {
    async login(username: string, password: string) {
      if (devBypassEnabled) {
        this.username = devBypassUser.username
        this.role = devBypassUser.role
        this.ready = true
        return true
      }
      const r = await authApi.login(username, password)
      this.username = r.username
      this.role = r.role
      this.ready = true
      return true
    },
    async refresh(): Promise<boolean> {
      if (devBypassEnabled) {
        this.username = devBypassUser.username
        this.role = devBypassUser.role
        this.ready = true
        return true
      }
      try {
        const r = await authApi.me()
        this.username = r.username
        this.role = r.role
        return true
      } catch {
        this.username = ''
        this.role = ''
        return false
      } finally {
        this.ready = true
      }
    },
    async logout() {
      try { await authApi.logout() } catch {}
      this.username = ''
      this.role = ''
    },
  },
})
