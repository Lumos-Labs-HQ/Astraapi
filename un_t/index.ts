import { Hono } from 'hono'
import { serve } from 'bun'

const app = new Hono()

app.get('/', (c) => {
  return c.text('Hello Hono + Bun 🚀')
})

// 👇 THIS is what you want (Bun server)
serve({
  port: 3000,
  fetch: app.fetch,
})

console.log('Running on http://localhost:3000')