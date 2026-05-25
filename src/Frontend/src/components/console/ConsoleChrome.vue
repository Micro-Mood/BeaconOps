<script setup lang="ts">
import ConsoleTopbar from '@/components/console/ConsoleTopbar.vue'
import WelcomeBanner from '@/components/console/WelcomeBanner.vue'

const props = withDefaults(defineProps<{
  theme: 'light' | 'dark'
  loading?: boolean
}>(), {
  loading: false,
})

const emit = defineEmits<{
  (e: 'change-theme', theme: 'light' | 'dark'): void
}>()
</script>

<template>
  <div class="console-root" :class="[`theme-${props.theme}`, { 'is-loading': props.loading }]">
    <div class="console-stage">
      <ConsoleTopbar :theme="props.theme" @change-theme="emit('change-theme', $event)" />
      <WelcomeBanner />

      <slot />
    </div>
  </div>
</template>

<style scoped>
.console-root {
  --bo-topbar-max: 1360px;
  min-height: 100vh;
  width: 100%;
  padding: 18px 18px 28px;
  overflow: auto;
  box-sizing: border-box;
  background:
    radial-gradient(circle at top left, rgba(24, 58, 66, 0.12), transparent 34%),
    linear-gradient(180deg, rgba(255, 255, 255, 0.72), rgba(255, 255, 255, 0.18)),
    var(--bo-bg);
  color: var(--bo-text);
  transition: background var(--bo-transition), color var(--bo-transition);
}

.console-root.theme-dark {
  --bo-bg: #091418;
  --bo-bg-soft: rgba(11, 21, 24, 0.9);
  --bo-bg-panel: rgba(13, 22, 26, 0.88);
  --bo-border: rgba(225, 236, 238, 0.14);
  --bo-border-strong: rgba(225, 236, 238, 0.28);
  --bo-text: #edf5f5;
  --bo-text-mute: rgba(237, 245, 245, 0.72);
  --bo-console-shell: #0f2a31;
  --bo-console-button-glass: rgba(10, 24, 28, 0.76);
  --bo-console-button-glass-border: rgba(237, 245, 245, 0.2);
  --bo-console-button-glass-shadow: rgba(0, 0, 0, 0.26);
  --bo-console-text: #edf5f5;
  --bo-console-text-soft: rgba(237, 245, 245, 0.76);
  --bo-console-text-muted: rgba(237, 245, 245, 0.64);
  --bo-console-text-faint: rgba(237, 245, 245, 0.54);
  --bo-console-line: rgba(237, 245, 245, 0.16);
  --bo-console-input-line: rgba(237, 245, 245, 0.26);
  background:
    radial-gradient(circle at top left, rgba(70, 126, 138, 0.26), transparent 36%),
    linear-gradient(180deg, rgba(7, 16, 19, 0.94), rgba(7, 16, 19, 0.84)),
    var(--bo-bg);
}

.console-stage {
  width: min(calc(100vw - 36px), var(--bo-topbar-max));
  margin: 0 auto;
  display: grid;
  gap: 30px;
}

@media (max-width: 900px) {
  .console-root {
    padding: 10px 0 24px;
    overflow-x: hidden;
  }

  .console-stage {
    width: 100vw;
    gap: 16px;
  }
}
</style>