<script setup lang="ts">
import BeaconDeviceFrame from '@/components/beacon/BeaconDeviceFrame.vue'
import type { ShellTone } from '@/lib/console'

const props = withDefaults(defineProps<{
  tone?: ShellTone
}>(), {
  tone: 'normal',
})
</script>

<template>
  <div class="auth-device-page">
    <BeaconDeviceFrame :tone="props.tone" class="auth-device-frame">
      <div class="auth-screen-surface">
        <slot />
      </div>
    </BeaconDeviceFrame>
  </div>
</template>

<style scoped>
.auth-device-page {
  display: grid;
  place-items: start center;
  min-height: 0;
  width: 100%;
  margin-top: -64px;
  overflow: visible;
}

.auth-device-frame {
  --bo-beacon-width: var(--bo-console-shell-base-width);
  transform: scale(var(--bo-console-shell-scale));
  transform-origin: top center;
}

.auth-screen-surface {
  width: 100%;
  height: 100%;
  padding: 18px 22px;
  color: var(--bo-console-text);
}

@media (max-width: 900px) {
  .auth-device-page {
    margin-top: -38px;
    height: 102vw;
  }

  .auth-device-frame {
    --bo-beacon-actual-width: 100vw;
    --bo-screen-design-width: 104vw;
    --bo-screen-design-height: 56vw;
    transform: translateX(-21.46vw) scale(1.45);
    transform-origin: top left;
  }
}

@container beacon-shell (max-width: 900px) {
  .auth-screen-surface {
    padding: 12px 14px;
  }
}
</style>