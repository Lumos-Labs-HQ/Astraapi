import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'AstraAPI',
  titleTemplate: ':title — AstraAPI',
  description: 'The FastAPI-compatible framework powered by C++. Built-in workers, zero-copy I/O, no external server needed.',
  lang: 'en-US',

  head: [
    ['link', { rel: 'icon', type: 'image/png', href: '/icon.png' }],
    ['meta', { name: 'theme-color', content: '#00b894' }],
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: 'AstraAPI' }],
    ['meta', { property: 'og:title', content: 'AstraAPI — FastAPI-compatible, C++ powered, built-in workers' }],
    ['meta', { property: 'og:description', content: 'AstraAPI is a FastAPI-compatible Python web framework with a C++ core, built-in multi-worker server, and zero-copy HTTP pipeline. No gunicorn or uvicorn required.' }],
    ['meta', { property: 'og:image', content: '/icon.png' }],
    ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
    ['meta', { name: 'twitter:image', content: '/icon.png' }],
  ],

  themeConfig: {
    logo: { src: '/icon.png', alt: 'AstraAPI' },
    siteTitle: 'AstraAPI',

    search: {
      provider: 'local',
    },

    editLink: {
      pattern: 'https://github.com/your-org/astraapi/edit/main/docs/:path',
      text: 'Edit this page on GitHub',
    },

    nav: [
      { text: 'Guide', link: '/guide/getting-started', activeMatch: '/guide/' },
      { text: 'Architecture', link: '/architecture/', activeMatch: '/architecture/' },
      { text: 'Features', link: '/features/routing', activeMatch: '/features/' },
      { text: 'Performance', link: '/performance/benchmarks', activeMatch: '/performance/' },
      { text: 'Examples', link: '/examples/hello-world', activeMatch: '/examples/' },
    ],

    sidebar: {
      '/guide/': [
        {
          text: 'Getting Started',
          items: [
            { text: 'Why AstraAPI?', link: '/guide/getting-started' },
            { text: 'Installation', link: '/guide/installation' },
            { text: 'Quick Start', link: '/guide/quickstart' },
          ],
        },
        {
          text: 'Fundamentals',
          items: [
            { text: 'Create an App', link: '/guide/create-app' },
            { text: 'Run the Server', link: '/guide/run-server' },
            { text: 'Project Structure', link: '/guide/project-structure' },
          ],
        },
      ],
      '/architecture/': [
        {
          text: 'Architecture',
          items: [
            { text: 'Overview', link: '/architecture/' },
            { text: 'C++ Core', link: '/architecture/cpp-core' },
            { text: 'Python Asyncio Bridge', link: '/architecture/python-bridge' },
            { text: 'HTTP Pipeline', link: '/architecture/http-pipeline' },
            { text: 'Memory Model', link: '/architecture/memory-model' },
            { text: 'Zero-Copy & Caching', link: '/architecture/zero-copy' },
          ],
        },
      ],
      '/features/': [
        {
          text: 'Routing',
          items: [
            { text: 'Basic Routing', link: '/features/routing' },
            { text: 'Path Parameters', link: '/features/path-params' },
            { text: 'Query Parameters', link: '/features/query-params' },
          ],
        },
        {
          text: 'Request',
          items: [
            { text: 'Request Body', link: '/features/request-body' },
            { text: 'Headers & Cookies', link: '/features/headers-cookies' },
            { text: 'Form Data', link: '/features/form-data' },
            { text: 'File Uploads', link: '/features/file-uploads' },
          ],
        },
        {
          text: 'Response',
          items: [
            { text: 'JSON & HTML Responses', link: '/features/responses' },
            { text: 'Streaming Response', link: '/features/streaming' },
            { text: 'WebSockets', link: '/features/websockets' },
            { text: 'Background Tasks', link: '/features/background-tasks' },
          ],
        },
        {
          text: 'Advanced',
          items: [
            { text: 'Dependency Injection', link: '/features/dependencies' },
            { text: 'Middleware', link: '/features/middleware' },
            { text: 'Exception Handling', link: '/features/exception-handling' },
            { text: 'Validation', link: '/features/validation' },
            { text: 'Security', link: '/features/security' },
            { text: 'CORS', link: '/features/cors' },
            { text: 'GZip Compression', link: '/features/gzip' },
            { text: 'Static Files', link: '/features/static-files' },
            { text: 'OpenAPI & Docs', link: '/features/openapi-docs' },
          ],
        },
      ],
      '/performance/': [
        {
          text: 'Performance',
          items: [
            { text: 'Benchmarks', link: '/performance/benchmarks' },
            { text: 'Optimization Guide', link: '/performance/optimization' },
          ],
        },
      ],
      '/testing/': [
        {
          text: 'Testing',
          items: [
            { text: 'Test Client', link: '/testing/' },
            { text: 'Async Testing', link: '/testing/async-tests' },
            { text: 'Database Testing', link: '/testing/database-tests' },
          ],
        },
      ],
      '/deployment/': [
        {
          text: 'Deployment',
          items: [
            { text: 'Deploy with Docker', link: '/deployment/docker' },
            { text: 'Built-in Workers', link: '/deployment/workers' },
            { text: 'Production Checklist', link: '/deployment/production-checklist' },
          ],
        },
      ],
      '/examples/': [
        {
          text: 'Examples',
          items: [
            { text: 'Hello World', link: '/examples/hello-world' },
            { text: 'CRUD API', link: '/examples/crud-api' },
            { text: 'Real-time Chat', link: '/examples/realtime-chat' },
          ],
        },
      ],
      '/contributing/': [
        {
          text: 'Contributing',
          items: [
            { text: 'Contributing Guide', link: '/contributing/' },
          ],
        },
      ],
    },

    socialLinks: [
      { icon: 'github', link: 'https://github.com/your-org/astraapi' },
    ],

    footer: {
      message: 'Released under the MIT License.',
      copyright: 'Copyright © 2025 AstraAPI Contributors',
    },
  },

  ignoreDeadLinks: true,

  markdown: {
    theme: {
      light: 'github-light',
      dark: 'github-dark',
    },
    lineNumbers: true,
  },
})
