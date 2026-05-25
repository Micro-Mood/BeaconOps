<script setup lang="ts">
import { useRouter } from 'vue-router'
import { useAuth } from '@/stores/auth'

const props = defineProps<{
  theme: 'light' | 'dark'
}>()

const router = useRouter()
const auth = useAuth()

const emit = defineEmits<{
  (e: 'change-theme', theme: 'light' | 'dark'): void
}>()

function setTheme(nextTheme: 'light' | 'dark') {
  emit('change-theme', nextTheme)
}

async function logout() {
  await auth.logout()
  router.push({ name: 'login' })
}
</script>

<template>
  <header class="console-topbar">
    <div class="topbar-brand">
      <img
        class="brand-logo"
        :class="{ 'brand-logo--dark': props.theme === 'dark' }"
        src="/logo.png"
        alt=""
        aria-hidden="true"
      />
      BeaconOps
    </div>

    <div class="topbar-actions">
      <a class="topbar-link" href="https://github.com/Micro-Mood/BeaconOps" target="_blank" rel="noreferrer">GitHub</a>
      <button v-if="auth.isLogged" type="button" class="topbar-link topbar-button" @click="logout">退出</button>

      <div class="theme-switch" aria-label="主题切换">
        <button
          type="button"
          class="theme-chip"
          :class="{ active: props.theme === 'light' }"
          @click="setTheme('light')"
        >
          亮色
        </button>
        <button
          type="button"
          class="theme-chip"
          :class="{ active: props.theme === 'dark' }"
          @click="setTheme('dark')"
        >
          暗色
        </button>
      </div>
    </div>
  </header>
</template>

<style scoped>
.console-topbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 18px;
}

.topbar-brand {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: clamp(20px, 3vw, 30px);
  line-height: 1;
  font-weight: 700;
  letter-spacing: 0.02em;
}

.brand-logo {
  height: 1.2em;
  width: auto;
  display: block;
  /* black lines on transparent: works on light background */
  filter: brightness(0);
}

.brand-logo--dark {
  /* invert to white for dark background */
  filter: brightness(0) invert(1);
}

.topbar-actions {
  display: flex;
  align-items: center;
  justify-self: end;
  gap: 14px;
}

.topbar-link {
  color: var(--bo-text);
  font-size: 15px;
  line-height: 1;
}

.topbar-button {
  padding: 0;
  border: none;
  background: none;
  font: inherit;
  cursor: pointer;
}

.theme-switch {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding-left: 14px;
  border-left: 1px solid var(--bo-border-strong);
}

.theme-chip {
  border: 1px solid transparent;
  background: transparent;
  color: var(--bo-text-mute);
  padding: 8px 14px;
  border-radius: 999px;
  cursor: pointer;
  font-size: 14px;
  line-height: 1;
  transition: color var(--bo-transition), border-color var(--bo-transition), background var(--bo-transition);
}

.theme-chip.active {
  color: var(--bo-text);
  border-color: var(--bo-border-strong);
  background: rgba(255, 255, 255, 0.12);
}

@media (max-width: 900px) {
  .console-topbar {
    align-items: flex-start;
    flex-direction: column;
    padding: 0 18px;
  }

  .topbar-actions {
    justify-content: flex-start;
  }

  .theme-switch {
    padding-left: 10px;
  }
}
</style>