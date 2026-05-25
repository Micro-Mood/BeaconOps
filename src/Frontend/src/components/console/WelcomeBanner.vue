<template>
  <section class="welcome-banner" aria-label="Welcome 欢迎回来">
    <div class="welcome-cycle">
      <span class="welcome-line welcome-line-en" aria-hidden="true">Welcome</span>
      <span class="welcome-line welcome-line-zh" aria-hidden="true">欢迎回来</span>
    </div>
  </section>
</template>

<style scoped>
.welcome-banner {
  display: grid;
  align-items: center;
  justify-items: center;
  min-height: clamp(84px, 16vw, 138px);
  text-align: center;
}

.welcome-cycle {
  display: inline-grid;
  justify-self: center;
  place-items: center;
  min-height: 1.1em;
  width: max-content;
  max-width: 100%;
  padding: 6px 0;
  font-size: clamp(44px, 8.6vw, 92px);
  line-height: 1;
  font-weight: 700;
  letter-spacing: 0;
  overflow: hidden;
  transform: translateY(10px);
}

.welcome-line {
  grid-area: 1 / 1;
  white-space: nowrap;
  opacity: 0;
  clip-path: inset(0 100% 0 0);
  filter: blur(10px);
  transform: translate3d(-20px, 0, 0);
  will-change: opacity, clip-path, transform, filter;
  animation: welcome-swap 8s cubic-bezier(0.22, 1, 0.36, 1) infinite;
}

.welcome-line-zh {
  animation-delay: 4s;
}

@keyframes welcome-swap {
  0% {
    opacity: 0;
    clip-path: inset(0 100% 0 0);
    filter: blur(10px);
    transform: translate3d(-20px, 0, 0);
  }

  12.5% {
    opacity: 1;
    clip-path: inset(0 0 0 0);
    filter: blur(0);
    transform: translate3d(0, 0, 0);
  }

  37.5% {
    opacity: 1;
    clip-path: inset(0 0 0 0);
    filter: blur(0);
    transform: translate3d(0, 0, 0);
  }

  50% {
    opacity: 0;
    clip-path: inset(0 100% 0 0);
    filter: blur(8px);
    transform: translate3d(10px, 0, 0);
  }

  100% {
    opacity: 0;
    clip-path: inset(0 100% 0 0);
    filter: blur(8px);
    transform: translate3d(10px, 0, 0);
  }
}

@media (max-width: 900px) {
  .welcome-banner {
    width: 100vw;
    min-height: 54px;
    justify-items: stretch;
    overflow: hidden;
  }

  .welcome-cycle {
    display: grid;
    grid-template-columns: minmax(0, 1fr);
    justify-items: center;
    justify-content: center;
    width: 100%;
    font-size: clamp(34px, 10vw, 44px);
    transform: none;
  }

  .welcome-line {
    justify-self: center;
  }
}
</style>