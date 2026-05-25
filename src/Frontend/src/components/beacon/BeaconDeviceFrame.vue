<script setup lang="ts">
import { computed } from 'vue'
import shellSrc from '@/assets/beacon-shell.svg'

const props = withDefaults(defineProps<{
  variant?: 'large' | 'mini'
  level?: string
  state?: string
  online?: boolean
  tone?: 'normal' | 'loading' | 'info' | 'success' | 'warning' | 'error'
}>(), {
  variant: 'large',
  level: 'info',
  state: '',
  online: true,
  tone: undefined,
})

const toneClass = computed(() => {
  if (props.tone) return `tone-${props.tone}`
  if (props.state === 'loading') return 'tone-loading'
  if (props.level === 'emergency') return 'tone-error'
  if (props.level === 'warn') return 'tone-warning'
  if (props.level === 'notice') return 'tone-success'
  if (props.level === 'info') return 'tone-info'
  if (!props.online) return 'tone-warning'
  return 'tone-normal'
})

const shellMaskStyle = computed(() => ({
  maskImage: `url('${shellSrc}')`,
  WebkitMaskImage: `url('${shellSrc}')`,
}))
</script>

<template>
  <div
    class="beacon-device"
    :class="[`is-${props.variant}`, toneClass, `level-${props.level || 'info'}`, `state-${props.state || 'idle'}`, { 'is-offline': !props.online }]"
  >
    <div class="device-stage">
      <div class="shell-mask" :style="shellMaskStyle" aria-hidden="true"></div>

      <div class="device-screen-slot">
        <div class="screen-scaler">
          <slot />
        </div>
      </div>
    </div>

    <slot name="caption" />
  </div>
</template>

<style scoped>
.beacon-device {
  --shell-color: var(--bo-console-shell);
  --screen-bg: transparent;
  position: relative;
  width: var(--bo-beacon-actual-width, min(var(--bo-beacon-width, 1160px), 100%));
  margin: 0 auto;
  color: var(--shell-color);
  container-type: inline-size;
  container-name: beacon-shell;
}

.beacon-device.tone-loading .shell-img,
.beacon-device.tone-info .shell-img,
.beacon-device.tone-success .shell-img,
.beacon-device.tone-warning .shell-img,
.beacon-device.tone-error .shell-img {
  /* shell stays black; state expressed via screen background */
}

.device-stage {
  position: relative;
  width: 100%;
  aspect-ratio: 595.2 / 419.52;
}

.shell-mask {
  position: absolute;
  inset: 0;
  z-index: 0;
  width: 100%;
  height: 100%;
  background-color: var(--shell-color);
  mask-size: 100% 100%;
  -webkit-mask-size: 100% 100%;
  filter: drop-shadow(0 10px 18px rgba(15, 23, 42, 0.08));
}



.device-screen-slot {
  position: absolute;
  z-index: 2;
  /* SVG thick-stroke screen rect in viewBox 595.2x419.52. */
  left: 22.18%;
  top: 32.54%;
  width: 54.10%;
  height: 41.44%;
  overflow: hidden;
  background: var(--screen-bg);
  color: var(--bo-console-text);
  container-type: size;
  container-name: beacon-screen;
}

.screen-scaler {
  width: var(--bo-screen-design-width, 627px);
  height: var(--bo-screen-design-height, 339px);
  transform-origin: top left;
  transform: scale(calc(100cqw / var(--bo-screen-design-width, 627px)));
}

.beacon-device.is-mini .screen-scaler {
  /* mini variant 不参与桌面设计尺寸的缩放,保持原状 */
  width: 100%;
  height: 100%;
  transform: none;
}

.beacon-device.is-mini {
  width: 100%;
  max-width: 320px;
}

.beacon-device.is-mini .shell-mask {
  filter: drop-shadow(0 8px 12px rgba(15, 23, 42, 0.06));
}

.beacon-device.is-mini .device-screen-slot {
  border-radius: 8px;
}

</style>