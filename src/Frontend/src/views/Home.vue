<script setup lang="ts">
import { useRouter } from 'vue-router'
import ConsoleShell from '@/components/console/ConsoleShell.vue'
import { useStream } from '@/stores/stream'

const router = useRouter()
const stream = useStream()
</script>

<template>
  <ConsoleShell
    active-main="home"
    :tone="stream.connected ? 'normal' : 'warning'"
    title="BeaconOps"
  >
    <div class="home-copy">
      <p class="hero-line">一套面向 Beacon 终端的消息发布与设备管理控制台</p>
    </div>

    <div class="divider" />

    <div class="home-visual" aria-label="消息下发到 Beacon 终端并形成回执">
      <div class="message-card" aria-hidden="true">
        <span></span>
        <span></span>
        <span></span>
      </div>
      <div class="signal-wave" aria-hidden="true">
        <span></span>
        <span></span>
        <span></span>
      </div>
      <div class="terminal-stack" aria-hidden="true">
        <span class="terminal terminal-back"></span>
        <span class="terminal terminal-front">
          <i></i>
        </span>
      </div>
    </div>

    <template #footer>
      <button class="screen-btn" @click="router.push({ name: 'send' })">去发送</button>
    </template>
  </ConsoleShell>
</template>

<style scoped>
.home-copy {
  display: grid;
  margin-bottom: 2px;
}

.hero-line {
  margin: 0;
  font-size: clamp(18px, 4cqi, 24px);
  line-height: 1.35;
  font-weight: 700;
}

.home-visual {
  position: relative;
  display: grid;
  grid-template-columns: 1fr 74px 1fr;
  align-items: center;
  gap: 14px;
  min-height: 118px;
  padding: 10px 6px 2px;
}

.message-card,
.terminal-front,
.terminal-back {
  border: 2px solid var(--bo-console-shell);
  background: rgba(255, 255, 255, 0.72);
}

.message-card {
  justify-self: end;
  width: min(112px, 100%);
  height: 70px;
  display: grid;
  align-content: center;
  gap: 8px;
  padding: 14px;
  border-radius: 14px;
}

.message-card span {
  display: block;
  height: 5px;
  border-radius: 999px;
  background: var(--bo-console-shell);
  opacity: 0.72;
}

.message-card span:nth-child(2) {
  width: 76%;
}

.message-card span:nth-child(3) {
  width: 48%;
}

.signal-wave {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  align-items: center;
  gap: 7px;
}

.signal-wave span {
  display: block;
  height: 44px;
  border: 2px solid var(--bo-console-shell);
  border-left: none;
  border-radius: 0 999px 999px 0;
  opacity: 0.28;
}

.signal-wave span:nth-child(2) {
  height: 64px;
  opacity: 0.48;
}

.signal-wave span:nth-child(3) {
  height: 84px;
  opacity: 0.68;
}

.terminal-stack {
  position: relative;
  justify-self: start;
  width: 122px;
  height: 82px;
}

.terminal {
  position: absolute;
  display: block;
  border-radius: 16px;
}

.terminal-back {
  inset: 4px 0 0 16px;
  opacity: 0.3;
}

.terminal-front {
  inset: 0 14px 10px 0;
}

.terminal-front::before,
.terminal-front::after {
  content: '';
  position: absolute;
  left: 18px;
  right: 18px;
  height: 4px;
  border-radius: 999px;
  background: var(--bo-console-shell);
  opacity: 0.64;
}

.terminal-front::before {
  top: 20px;
}

.terminal-front::after {
  top: 34px;
  right: 42px;
}

.terminal-front i {
  position: absolute;
  right: 13px;
  bottom: 11px;
  width: 18px;
  height: 10px;
  border-left: 3px solid var(--bo-console-shell);
  border-bottom: 3px solid var(--bo-console-shell);
  transform: rotate(-45deg);
}

@container beacon-shell (max-width: 900px) {
  .hero-line {
    font-size: 17px;
  }

  .home-visual {
    grid-template-columns: 1fr 48px 1fr;
    gap: 8px;
  }

  .message-card {
    height: 58px;
    padding: 11px;
  }

  .terminal-stack {
    width: 94px;
    height: 66px;
  }
}
</style>
