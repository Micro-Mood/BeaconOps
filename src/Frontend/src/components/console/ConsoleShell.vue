<script setup lang="ts">
import { computed } from 'vue'
import { useRouter } from 'vue-router'
import BeaconDeviceFrame from '@/components/beacon/BeaconDeviceFrame.vue'
import type { RouteLocationRaw } from 'vue-router'
import type { ShellTone } from '@/lib/console'

export interface MenuItem {
  key: string
  label: string
  to?: RouteLocationRaw
  action?: () => void
  active?: boolean
  danger?: boolean
}

interface FooterAction {
  key: string
  label: string
  tone?: 'primary' | 'text'
  disabled?: boolean
  action: () => void | Promise<void>
}

const props = withDefaults(defineProps<{
  activeMain: string
  secondaryItems?: MenuItem[]
  title?: string
  subtitle?: string
  tone?: ShellTone
  footerActions?: FooterAction[]
}>(), {
  secondaryItems: () => [],
  title: '',
  subtitle: '',
  tone: 'normal',
  footerActions: () => [],
})

const router = useRouter()

const mainItems: MenuItem[] = [
  { key: 'home', label: '首页', to: { name: 'home' } },
  { key: 'send', label: '发送', to: { name: 'send' } },
  { key: 'history', label: '历史', to: { name: 'history' } },
  { key: 'devices', label: '设备', to: { name: 'devices' } },
  { key: 'batches', label: '批次', to: { name: 'batches' } },
  { key: 'settings', label: '设置', to: { name: 'settings-admins' } },
]

const hasSecondary = computed(() => props.secondaryItems.length > 0)

function navigate(item: MenuItem) {
  if (item.action) {
    item.action()
    return
  }
  if (item.to) router.push(item.to)
}
</script>

<template>
  <div class="console-shell-page">
    <BeaconDeviceFrame :tone="tone" class="console-frame">
      <div class="console-screen-surface">
        <div class="console-grid" :class="{ 'has-secondary': hasSecondary }">
          <nav class="console-main-nav">
            <button
              v-for="item in mainItems"
              :key="item.key"
              type="button"
              class="nav-text"
              :class="{ active: item.key === activeMain }"
              @click="navigate(item)"
            >
              {{ item.label }}
            </button>
          </nav>

          <nav v-if="hasSecondary" class="console-sub-nav">
            <button
              v-for="item in secondaryItems"
              :key="item.key"
              type="button"
              class="nav-text sub-text"
              :class="{ active: item.active, danger: item.danger }"
              @click="navigate(item)"
            >
              {{ item.label }}
            </button>
          </nav>

          <section class="console-content">
            <header v-if="title || subtitle" class="screen-head">
              <h1 v-if="title">{{ title }}</h1>
              <p v-if="subtitle">{{ subtitle }}</p>
            </header>
            <footer id="console-screen-foot" class="screen-foot">
              <button
                v-for="action in footerActions"
                :key="action.key"
                type="button"
                :class="action.tone === 'text' ? 'text-action' : 'screen-btn'"
                :disabled="action.disabled"
                @click="action.action()"
              >
                {{ action.label }}
              </button>
              <slot name="footer" />
            </footer>
            <div class="screen-scroll">
              <slot />
            </div>
          </section>
        </div>
      </div>
    </BeaconDeviceFrame>
  </div>
</template>

<style scoped>
.console-shell-page {
  display: grid;
  place-items: start center;
  min-height: 0;
  width: 100%;
  margin-top: -64px;
  overflow: visible;
}

.console-frame {
  --bo-beacon-width: var(--bo-console-shell-base-width);
  transform: scale(var(--bo-console-shell-scale));
  transform-origin: top center;
}

.console-screen-surface {
  width: 100%;
  height: 100%;
  padding: 5px 7px;
  box-sizing: border-box;
}

.console-grid {
  display: grid;
  grid-template-columns: 82px minmax(0, 1fr);
  height: 100%;
}

.console-grid.has-secondary {
  grid-template-columns: 82px 82px minmax(0, 1fr);
}

.console-main-nav,
.console-sub-nav,
.console-content {
  min-width: 0;
  min-height: 0;
}

.console-main-nav,
.console-sub-nav {
  display: flex;
  flex-direction: column;
  justify-content: space-evenly;
  padding: 14px 9px;
  border-right: 2px solid var(--bo-console-shell);
}

.console-content {
  display: grid;
  grid-template-rows: auto minmax(0, 1fr);
  position: relative;
  padding: 14px 14px 10px;
}

.screen-head {
  padding-bottom: 6px;
}

.screen-head h1 {
  margin: 0;
  font-size: var(--bo-console-title-size, var(--bo-console-title-md));
  line-height: 1.08;
  font-weight: 700;
}

.screen-head p {
  margin: 3px 0 0;
  font-size: 13px;
  line-height: 1.4;
  color: var(--bo-console-text-soft);
}

.screen-scroll {
  min-height: 0;
  overflow: auto;
  padding-right: 8px;
}

.screen-foot:not(:empty) + .screen-scroll {
  padding-bottom: var(--bo-console-footer-safe-space, 76px);
}

.screen-scroll::-webkit-scrollbar {
  width: 8px;
}

.screen-scroll::-webkit-scrollbar-thumb {
  background: var(--bo-console-line);
  border-radius: 999px;
}

.screen-foot {
  position: absolute;
  bottom: 10px;
  right: 14px;
  z-index: 20;
  display: flex;
  gap: 10px;
  align-items: center;
  justify-content: flex-end;
}

.screen-foot:empty {
  display: none;
}

.nav-text {
  padding: 0;
  border: none;
  background: none;
  color: var(--bo-console-text-soft);
  text-align: center;
  font: inherit;
  font-size: clamp(16px, 4cqi, 25px);
  line-height: 1.25;
  cursor: pointer;
}

.nav-text.active {
  color: var(--bo-console-text);
  font-weight: 700;
}

.sub-text {
  font-size: clamp(16px, 4cqi, 25px);
}

.sub-text.danger {
  color: var(--bo-console-danger);
}

@media (max-width: 900px) {
  .console-shell-page {
    margin-top: -38px;
    width: 100%;
    height: 102vw;
  }

  .console-frame {
    /* Layout stays 100vw so this oversized shell does not move Welcome/Topbar.
       Visual scale 1.45 makes the SVG's 69% visible card span the viewport.
       Inner screen scale resolves to about 0.75, matching the desktop-relative type size. */
    --bo-beacon-actual-width: 100vw;
    --bo-screen-design-width: 104vw;
    --bo-screen-design-height: 56vw;
    transform: translateX(-21.46vw) scale(1.45);
    transform-origin: top left;
  }
}

@container beacon-shell (max-width: 900px) {
  .console-screen-surface {
    padding: 4px 5px;
  }

  .console-grid.has-secondary {
    grid-template-columns: 70px 64px minmax(0, 1fr);
  }

  .console-grid:not(.has-secondary) {
    grid-template-columns: 70px minmax(0, 1fr);
  }

  .console-main-nav,
  .console-sub-nav {
    padding: 10px 7px 7px;
    gap: 3px;
  }

  .console-content {
    padding: 10px 10px 8px;
  }

  .screen-foot:not(:empty) + .screen-scroll {
    padding-bottom: var(--bo-console-footer-safe-space-mobile, 66px);
  }

  .nav-text {
    font-size: clamp(15px, 5cqi, 22px);
  }

  .sub-text {
    font-size: clamp(15px, 5cqi, 22px);
  }
}
</style>
