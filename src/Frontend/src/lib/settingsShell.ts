import { inject, onUnmounted, provide, ref, watchEffect, type InjectionKey, type Ref } from 'vue'

export interface SettingsShellAction {
  key: string
  label: string
  tone?: 'primary' | 'text'
  disabled?: boolean
  action: () => void | Promise<void>
}

type SettingsShellOwner = symbol

interface SettingsShellController {
  footerActions: Ref<SettingsShellAction[]>
  setFooterActions: (owner: SettingsShellOwner, actions: SettingsShellAction[]) => void
  clearFooterActions: (owner: SettingsShellOwner) => void
}

const settingsShellKey: InjectionKey<SettingsShellController> = Symbol('settings-shell')

export function provideSettingsShell() {
  const footerActions = ref<SettingsShellAction[]>([])
  let currentOwner: SettingsShellOwner | null = null

  function setFooterActions(owner: SettingsShellOwner, actions: SettingsShellAction[]) {
    currentOwner = owner
    footerActions.value = actions
  }

  function clearFooterActions(owner: SettingsShellOwner) {
    if (currentOwner !== owner) {
      return
    }
    currentOwner = null
    footerActions.value = []
  }

  provide(settingsShellKey, { footerActions, setFooterActions, clearFooterActions })

  return { footerActions, clearFooterActions }
}

export function useSettingsShell(factory: () => SettingsShellAction[]) {
  const shell = inject(settingsShellKey, null)
  const owner: SettingsShellOwner = Symbol('settings-shell-owner')

  if (!shell) {
    return
  }

  watchEffect(() => {
    shell.setFooterActions(owner, factory())
  })

  onUnmounted(() => {
    shell.clearFooterActions(owner)
  })
}