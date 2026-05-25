import { onMounted, ref } from 'vue'

type ConsoleTheme = 'light' | 'dark'

const STORAGE_KEY = 'beaconops-console-theme'

export function useConsoleTheme() {
  const theme = ref<ConsoleTheme>('light')

  function applyTheme(nextTheme: ConsoleTheme) {
    theme.value = nextTheme
    localStorage.setItem(STORAGE_KEY, nextTheme)
  }

  onMounted(() => {
    const savedTheme = localStorage.getItem(STORAGE_KEY)
    if (savedTheme === 'light' || savedTheme === 'dark') {
      theme.value = savedTheme
    }
  })

  return {
    theme,
    applyTheme,
  }
}