import { useEffect, useState } from 'react'

type CpuCounters = Record<string, number>

type AgentMetrics = {
  cpu_counters: CpuCounters | null
  memory: { total_kib: number; available_kib: number } | null
  disk: { path: string; total_bytes: number; available_bytes: number } | null
  uptime_seconds: number | null
  processes: Array<{ pid: number; name: string }> | null
}

type CurrentMetrics = AgentMetrics & { captured_at: string }

type Snapshot = {
  id: number
  captured_at: string
  metrics: AgentMetrics
}

type HistoryResponse = {
  count: number
  snapshots: Snapshot[]
}

const numberFormat = new Intl.NumberFormat()

function formatGiB(value: number) {
  return `${(value / 1024 ** 3).toFixed(1)} GiB`
}

function formatKiB(value: number) {
  return `${(value / (1024 * 1024)).toFixed(1)} GiB`
}

function formatUptime(seconds: number) {
  const totalMinutes = Math.floor(seconds / 60)
  const days = Math.floor(totalMinutes / (24 * 60))
  const hours = Math.floor((totalMinutes % (24 * 60)) / 60)
  const minutes = totalMinutes % 60

  if (days > 0) return `${days}d ${hours}h ${minutes}m`
  return `${hours}h ${minutes}m`
}

function formatTimestamp(value: string) {
  return new Date(value).toLocaleString([], {
    month: 'short',
    day: 'numeric',
    hour: 'numeric',
    minute: '2-digit',
    second: '2-digit',
  })
}

function availablePercent(available: number, total: number) {
  if (total <= 0) return 0
  return Math.min(100, Math.max(0, (available / total) * 100))
}

async function readJson<T>(response: Response): Promise<T> {
  if (!response.ok) {
    let message = `Request failed (${response.status})`
    try {
      const body = (await response.json()) as { error?: string }
      if (body.error) message = body.error
    } catch {
      // Keep the HTTP status message when the response is not JSON.
    }
    throw new Error(message)
  }
  return response.json() as Promise<T>
}

function StatCard({
  label,
  value,
  detail,
  tone,
  ratio,
}: {
  label: string
  value: string
  detail: string
  tone: string
  ratio?: number
}) {
  return (
    <article className={`stat-card ${tone}`}>
      <div className="stat-heading">
        <span className="stat-marker" aria-hidden="true" />
        <h2>{label}</h2>
      </div>
      <p className="stat-value">{value}</p>
      <p className="stat-detail">{detail}</p>
      {ratio !== undefined && (
        <div
          className="capacity-track"
          role="meter"
          aria-label={`${label} available`}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={Math.round(ratio)}
        >
          <span style={{ width: `${ratio}%` }} />
        </div>
      )}
    </article>
  )
}

function App() {
  const [metrics, setMetrics] = useState<CurrentMetrics | null>(null)
  const [snapshots, setSnapshots] = useState<Snapshot[]>([])
  const [error, setError] = useState<string | null>(null)
  const [initialLoading, setInitialLoading] = useState(true)
  const [refreshing, setRefreshing] = useState(false)
  const [refreshKey, setRefreshKey] = useState(0)

  useEffect(() => {
    const controller = new AbortController()
    let active = true

    async function loadDashboard() {
      setRefreshing(true)
      try {
        const [metricsResponse, historyResponse] = await Promise.all([
          fetch('/api/metrics', { signal: controller.signal }),
          fetch('/api/history?limit=8', { signal: controller.signal }),
        ])
        const [nextMetrics, nextHistory] = await Promise.all([
          readJson<CurrentMetrics>(metricsResponse),
          readJson<HistoryResponse>(historyResponse),
        ])

        if (active) {
          setMetrics(nextMetrics)
          setSnapshots(nextHistory.snapshots)
          setError(null)
        }
      } catch (reason) {
        if (active && !controller.signal.aborted) {
          setError(reason instanceof Error ? reason.message : 'Unable to load system metrics')
        }
      } finally {
        if (active) {
          setInitialLoading(false)
          setRefreshing(false)
        }
      }
    }

    void loadDashboard()
    return () => {
      active = false
      controller.abort()
    }
  }, [refreshKey])

  useEffect(() => {
    const timer = window.setInterval(() => {
      setRefreshKey((current) => current + 1)
    }, 5000)
    return () => window.clearInterval(timer)
  }, [])

  const memory = metrics?.memory
  const disk = metrics?.disk
  const availableMemoryRatio = memory
    ? availablePercent(memory.available_kib, memory.total_kib)
    : undefined
  const availableDiskRatio = disk
    ? availablePercent(disk.available_bytes, disk.total_bytes)
    : undefined

  return (
    <main className="dashboard-shell">
      <header className="topbar">
        <div className="brand-lockup">
          <div className="brand-mark" aria-hidden="true">
            <span />
            <span />
            <span />
          </div>
          <div>
            <p className="eyebrow">SYSTEM MONITOR</p>
            <h1>SystemCare Shell</h1>
          </div>
        </div>
        <div className="header-actions">
          <div className={`connection-state ${error ? 'is-error' : metrics ? 'is-live' : ''}`}>
            <span className="connection-dot" />
            <span>{error ? 'Connection issue' : metrics ? 'Live' : 'Connecting'}</span>
          </div>
          <button
            className="refresh-button"
            type="button"
            onClick={() => setRefreshKey((current) => current + 1)}
            disabled={refreshing}
          >
            <span className={refreshing ? 'refresh-glyph is-spinning' : 'refresh-glyph'} aria-hidden="true">
              ↻
            </span>
            {refreshing ? 'Refreshing' : 'Refresh now'}
          </button>
        </div>
      </header>

      <section className="snapshot-heading" aria-label="Current snapshot">
        <div>
          <p className="eyebrow">CURRENT SNAPSHOT</p>
          <p className="snapshot-time">
            {metrics ? `Captured ${formatTimestamp(metrics.captured_at)}` : 'Waiting for first reading'}
          </p>
        </div>
        <p className="refresh-note">AUTO · 5 SEC</p>
      </section>

      {error && (
        <div className="error-banner" role="alert">
          <strong>Metrics unavailable.</strong> {error}.
        </div>
      )}

      {initialLoading && !metrics ? (
        <section className="loading-panel" aria-live="polite">
          <span className="loading-ring" />
          <div>
            <h2>Connecting to the system agent</h2>
            <p>Waiting for a live metrics snapshot.</p>
          </div>
        </section>
      ) : !metrics ? (
        <section className="empty-panel">
          <span className="empty-mark" aria-hidden="true">!</span>
          <h2>No system snapshot yet</h2>
          <p>Live system data is not available.</p>
        </section>
      ) : (
        <>
          <section className="stat-grid" aria-label="System overview">
            <StatCard
              label="Memory available"
              value={memory ? formatKiB(memory.available_kib) : '—'}
              detail={memory ? `of ${formatKiB(memory.total_kib)} total` : 'No memory reading'}
              ratio={availableMemoryRatio}
              tone="tone-mint"
            />
            <StatCard
              label="Disk available"
              value={disk ? formatGiB(disk.available_bytes) : '—'}
              detail={disk ? `of ${formatGiB(disk.total_bytes)} · ${disk.path}` : 'No disk reading'}
              ratio={availableDiskRatio}
              tone="tone-citrus"
            />
            <StatCard
              label="System uptime"
              value={metrics.uptime_seconds === null ? '—' : formatUptime(metrics.uptime_seconds)}
              detail={metrics.uptime_seconds === null ? 'No uptime reading' : `${numberFormat.format(Math.floor(metrics.uptime_seconds))} seconds`}
              tone="tone-lilac"
            />
            <StatCard
              label="Processes detected"
              value={metrics.processes ? numberFormat.format(metrics.processes.length) : '—'}
              detail={metrics.processes ? 'Visible in /proc' : 'No process reading'}
              tone="tone-peach"
            />
          </section>

          <section className="detail-grid">
            <article className="panel cpu-panel">
              <div className="panel-heading">
                <div>
                  <p className="eyebrow">/PROC/STAT</p>
                  <h2>CPU counters</h2>
                </div>
                <span className="unit-tag">USER_HZ</span>
              </div>
              {metrics.cpu_counters ? (
                <div className="counter-grid">
                  {Object.entries(metrics.cpu_counters).map(([name, value], index) => (
                    <div className="counter-cell" key={name} style={{ animationDelay: `${index * 35}ms` }}>
                      <span className="counter-name">{name.replace('_', ' ')}</span>
                      <strong>{numberFormat.format(value)}</strong>
                    </div>
                  ))}
                </div>
              ) : (
                <p className="muted-copy">CPU counters are unavailable in this snapshot.</p>
              )}
              <p className="panel-footnote">Cumulative kernel counters · ticks since boot</p>
            </article>

            <article className="panel history-panel">
              <div className="panel-heading">
                <div>
                  <p className="eyebrow">SAVED READINGS</p>
                  <h2>Recent history</h2>
                </div>
                <span className="count-tag">{snapshots.length} snapshots</span>
              </div>
              {snapshots.length > 0 ? (
                <div className="history-table-wrap">
                  <table>
                    <thead>
                      <tr>
                        <th>Captured</th>
                        <th>Mem. free</th>
                        <th>Processes</th>
                        <th>CPU idle</th>
                      </tr>
                    </thead>
                    <tbody>
                      {snapshots.map((snapshot) => (
                        <tr key={snapshot.id}>
                          <td>{formatTimestamp(snapshot.captured_at)}</td>
                          <td>{snapshot.metrics.memory ? formatKiB(snapshot.metrics.memory.available_kib) : '—'}</td>
                          <td>{snapshot.metrics.processes ? numberFormat.format(snapshot.metrics.processes.length) : '—'}</td>
                          <td>{snapshot.metrics.cpu_counters?.idle === undefined ? '—' : numberFormat.format(snapshot.metrics.cpu_counters.idle)}</td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              ) : (
                <div className="history-empty">
                  <span aria-hidden="true">↳</span>
                  <p>{refreshing ? 'Loading saved readings…' : 'No saved readings yet.'}</p>
                </div>
              )}
              <p className="panel-footnote">Newest first</p>
            </article>
          </section>
        </>
      )}

      <footer className="page-footer">
        <span>SystemCare Shell</span>
        <span>Live status</span>
      </footer>
    </main>
  )
}

export default App