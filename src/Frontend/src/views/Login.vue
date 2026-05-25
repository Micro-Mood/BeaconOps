<script setup lang="ts">
import { ref } from 'vue'
import { useRouter, useRoute } from 'vue-router'
import { ElMessage } from 'element-plus'
import AuthDeviceShell from '@/components/auth/AuthDeviceShell.vue'
import { useAuth } from '@/stores/auth'

const router = useRouter()
const route = useRoute()
const auth = useAuth()
const username = ref('')
const password = ref('')
const loading = ref(false)

async function submit() {
  if (!username.value || !password.value) {
    ElMessage.warning('请输入用户名和密码')
    return
  }
  loading.value = true
  try {
    await auth.login(username.value.trim(), password.value)
    const redir = (route.query.r as string) || '/send'
    router.replace(redir)
  } catch (e: any) {
    ElMessage.error(e?.response?.data?.detail || e?.message || '登录失败')
  } finally {
    loading.value = false
  }
}
</script>

<template>
  <AuthDeviceShell tone="normal">
    <section class="login-panel">
      <header class="login-head">
        <h1>BeaconOps</h1>
        <p class="login-intro">一套面向 Beacon 终端的消息发布与设备管理控制台</p>
      </header>

      <div class="divider" />

      <form class="login-form" @submit.prevent="submit">
        <label class="login-field">
          <span>用户名</span>
          <input v-model="username" autofocus autocomplete="username" placeholder="输入用户名" />
        </label>
        <label class="login-field">
          <span>密码</span>
          <input v-model="password" type="password" autocomplete="current-password" placeholder="输入密码" />
        </label>

        <div class="login-actions">
          <button class="screen-btn" type="submit" :disabled="loading">
            {{ loading ? '登录中' : '登录' }}
          </button>
        </div>
      </form>
    </section>
  </AuthDeviceShell>
</template>

<style scoped>
.login-panel {
  position: relative;
  display: grid;
  grid-template-rows: auto auto minmax(0, 1fr);
  gap: 12px;
  height: 100%;
  align-content: center;
}

.login-head {
  display: grid;
  gap: 8px;
}

.login-head h1 {
  margin: 0;
  color: var(--bo-console-text);
  font-size: clamp(30px, 9cqi, 52px);
  line-height: 1;
  font-weight: 700;
}

.login-intro {
  margin: 0;
  max-width: 360px;
  color: var(--bo-console-text-soft);
  font-size: 16px;
  line-height: 1.45;
}

.login-form {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-content: start;
  gap: 14px 18px;
  width: 100%;
  padding-bottom: 74px;
}

.login-field {
  display: grid;
  gap: 5px;
  color: var(--bo-console-text-muted);
  font-size: 13px;
  font-weight: 700;
}

.login-field input {
  width: 100%;
  border: none;
  border-bottom: 2px solid var(--bo-console-input-line);
  border-radius: 0;
  background: transparent;
  color: var(--bo-console-text);
  font: inherit;
  font-size: 18px;
  line-height: 1.2;
  padding: 5px 0 7px;
  outline: none;
}

.login-field input:focus {
  border-bottom-color: var(--bo-console-shell);
}

.login-actions {
  position: absolute;
  right: 0;
  bottom: 0;
}

@container beacon-shell (max-width: 900px) {
  .login-panel {
    gap: 8px;
  }

  .login-head h1 {
    font-size: 40px;
  }

  .login-intro {
    max-width: 340px;
    font-size: 16px;
    line-height: 1.35;
  }

  .login-form {
    grid-template-columns: repeat(2, minmax(0, 1fr));
    gap: 12px 16px;
    padding-bottom: 52px;
  }

  .login-field {
    font-size: 13px;
  }

  .login-field input {
    font-size: 18px;
  }

  .login-actions .screen-btn {
    min-width: 94px;
    padding: 9px 16px;
    font-size: 17px;
  }
}
</style>
