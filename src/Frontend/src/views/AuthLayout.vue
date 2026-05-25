<script setup lang="ts">
import ConsoleChrome from '@/components/console/ConsoleChrome.vue'
import { useConsoleTheme } from '@/composables/useConsoleTheme'

const { theme, applyTheme } = useConsoleTheme()
</script>

<template>
  <ConsoleChrome :theme="theme" @change-theme="applyTheme">
    <router-view />
  </ConsoleChrome>
</template>