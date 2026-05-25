<script setup lang="ts">
import { onMounted } from 'vue'
import ConsoleChrome from '@/components/console/ConsoleChrome.vue'
import { useConsoleTheme } from '@/composables/useConsoleTheme'
import { useStream } from '@/stores/stream'

const stream = useStream()
const { theme, applyTheme } = useConsoleTheme()

onMounted(() => {
  stream.start()
})
</script>

<template>
  <ConsoleChrome :theme="theme" :loading="!stream.connected" @change-theme="applyTheme">
    <router-view v-slot="{ Component }">
      <component :is="Component" />
    </router-view>
  </ConsoleChrome>
</template>
