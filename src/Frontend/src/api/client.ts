import axios from 'axios'
import router from '@/router'

export const API_BASE = '/beacon/api/v1'

const http = axios.create({
  baseURL: API_BASE,
  withCredentials: true,
  timeout: 20000,
})

function readCookie(name: string): string {
  const prefix = `${name}=`
  return document.cookie
    .split(';')
    .map(part => part.trim())
    .find(part => part.startsWith(prefix))
    ?.slice(prefix.length) || ''
}

http.interceptors.request.use((config) => {
  const method = (config.method || 'get').toUpperCase()
  if (['POST', 'PUT', 'PATCH', 'DELETE'].includes(method)) {
    const csrf = readCookie('beacon_csrf')
    if (csrf) config.headers.set('X-CSRF-Token', decodeURIComponent(csrf))
  }
  return config
})

http.interceptors.response.use(
  (r) => r,
  (err) => {
    const status = err.response?.status
    if (status === 401) {
      const cur = router.currentRoute.value
      if (cur.name !== 'login') {
        router.replace({ name: 'login', query: { r: cur.fullPath } })
      }
    }
    return Promise.reject(err)
  },
)

export default http
