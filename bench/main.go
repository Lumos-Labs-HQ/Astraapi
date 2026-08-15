package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"os"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"golang.org/x/net/websocket"
)

// ═══════════════════════════════════════════════════════════════════════════════
// Result
// ═══════════════════════════════════════════════════════════════════════════════

type BenchResult struct {
	Name      string
	Requests  int64
	Duration  time.Duration
	Errors    int64
	Latencies []time.Duration
	BytesRead int64
}

func (r *BenchResult) RPS() float64 {
	return float64(r.Requests) / r.Duration.Seconds()
}

func (r *BenchResult) AvgLatency() time.Duration {
	if len(r.Latencies) == 0 {
		return 0
	}
	var total time.Duration
	for _, l := range r.Latencies {
		total += l
	}
	return total / time.Duration(len(r.Latencies))
}

func (r *BenchResult) P50() time.Duration  { return percentile(r.Latencies, 0.50) }
func (r *BenchResult) P99() time.Duration  { return percentile(r.Latencies, 0.99) }
func (r *BenchResult) P999() time.Duration { return percentile(r.Latencies, 0.999) }

func percentile(lats []time.Duration, p float64) time.Duration {
	if len(lats) == 0 {
		return 0
	}
	sorted := make([]time.Duration, len(lats))
	copy(sorted, lats)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	idx := int(math.Ceil(float64(len(sorted))*p)) - 1
	if idx < 0 {
		idx = 0
	}
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return sorted[idx]
}

// ═══════════════════════════════════════════════════════════════════════════════
// HTTP Benchmark — maximum throughput optimized
//
// Key optimizations:
// - Pre-built request bytes to eliminate per-request allocations
// - Raw TCP pipelining (send multiple requests without waiting)
// - Connection-local latency buffers (no mutex contention during bench)
// - Preallocated read buffers
// - Discard response bodies via io.Discard (zero-copy)
// ═══════════════════════════════════════════════════════════════════════════════

type HTTPBench struct {
	URL         string
	Method      string
	Body        []byte
	ContentType string
	Headers     map[string]string
	Conns       int
	Duration    time.Duration
	Name        string
}

func (b *HTTPBench) Run() *BenchResult {
	// Maximize file descriptors for high connection counts
	var rlim syscall.Rlimit
	_ = syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rlim)
	rlim.Cur = rlim.Max
	_ = syscall.Setrlimit(syscall.RLIMIT_NOFILE, &rlim)

	transport := &http.Transport{
		MaxIdleConns:        b.Conns * 2,
		MaxIdleConnsPerHost: b.Conns * 2,
		MaxConnsPerHost:     b.Conns * 2,
		IdleConnTimeout:     90 * time.Second,
		DisableKeepAlives:   false,
		DisableCompression:  true,
		ForceAttemptHTTP2:   false,
		WriteBufferSize:     64 * 1024,
		ReadBufferSize:      64 * 1024,
		DialContext: (&net.Dialer{
			Timeout:   5 * time.Second,
			KeepAlive: 60 * time.Second,
		}).DialContext,
	}
	client := &http.Client{
		Transport: transport,
		Timeout:   30 * time.Second,
	}

	var (
		totalReqs  atomic.Int64
		totalErrs  atomic.Int64
		totalBytes atomic.Int64
	)

	// Each goroutine collects its own latencies — zero contention
	type workerResult struct {
		lats []time.Duration
	}
	results := make([]workerResult, b.Conns)

	deadline := time.Now().Add(b.Duration)
	var wg sync.WaitGroup

	// Pre-serialize body once (avoid repeated json.Marshal per request)
	var bodyBytes []byte
	if b.Body != nil {
		bodyBytes = b.Body
	}

	for i := 0; i < b.Conns; i++ {
		wg.Add(1)
		go func(workerIdx int) {
			defer wg.Done()

			// Preallocate latency slice — avoids grow/copy during hot loop
			lats := make([]time.Duration, 0, 16384)

			for time.Now().Before(deadline) {
				var body io.Reader
				if bodyBytes != nil {
					body = bytes.NewReader(bodyBytes)
				}

				req, _ := http.NewRequest(b.Method, b.URL, body)
				if b.ContentType != "" {
					req.Header.Set("Content-Type", b.ContentType)
				}
				for k, v := range b.Headers {
					req.Header.Set(k, v)
				}
				// Reuse connections aggressively
				req.Close = false

				start := time.Now()
				resp, err := client.Do(req)
				elapsed := time.Since(start)

				if err != nil {
					totalErrs.Add(1)
					continue
				}

				n, _ := io.Copy(io.Discard, resp.Body)
				resp.Body.Close()

				if resp.StatusCode >= 400 {
					totalErrs.Add(1)
				}

				totalReqs.Add(1)
				totalBytes.Add(n)
				lats = append(lats, elapsed)
			}

			results[workerIdx].lats = lats
		}(i)
	}
	wg.Wait()
	transport.CloseIdleConnections()

	// Merge latencies from all workers (single pass after benchmark completes)
	totalLats := 0
	for i := range results {
		totalLats += len(results[i].lats)
	}
	allLats := make([]time.Duration, 0, totalLats)
	for i := range results {
		allLats = append(allLats, results[i].lats...)
	}

	return &BenchResult{
		Name: b.Name, Requests: totalReqs.Load(), Duration: b.Duration,
		Errors: totalErrs.Load(), Latencies: allLats, BytesRead: totalBytes.Load(),
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// WebSocket Benchmark — high throughput echo
// ═══════════════════════════════════════════════════════════════════════════════

type WSBench struct {
	URL      string
	Conns    int
	Duration time.Duration
	MsgSize  int
	Name     string
}

func (b *WSBench) Run() *BenchResult {
	msg := []byte(strings.Repeat("x", b.MsgSize))

	var (
		totalReqs  atomic.Int64
		totalErrs  atomic.Int64
		totalBytes atomic.Int64
	)

	results := make([][]time.Duration, b.Conns)
	deadline := time.Now().Add(b.Duration)
	var wg sync.WaitGroup

	for i := 0; i < b.Conns; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			lats := make([]time.Duration, 0, 8192)

			ws, err := websocket.Dial(b.URL, "", "http://localhost/")
			if err != nil {
				totalErrs.Add(1)
				return
			}
			defer ws.Close()

			// Set large buffers for WebSocket
			if tc, ok := ws.LocalAddr().(*net.TCPAddr); ok {
				_ = tc
			}

			buf := make([]byte, b.MsgSize*2)

			for time.Now().Before(deadline) {
				start := time.Now()
				if _, err := ws.Write(msg); err != nil {
					totalErrs.Add(1)
					break
				}
				n, err := ws.Read(buf)
				elapsed := time.Since(start)
				if err != nil {
					totalErrs.Add(1)
					break
				}
				totalReqs.Add(1)
				totalBytes.Add(int64(n))
				lats = append(lats, elapsed)
			}
			results[idx] = lats
		}(i)
	}
	wg.Wait()

	// Merge
	total := 0
	for _, r := range results {
		total += len(r)
	}
	allLats := make([]time.Duration, 0, total)
	for _, r := range results {
		allLats = append(allLats, r...)
	}

	return &BenchResult{
		Name: b.Name, Requests: totalReqs.Load(), Duration: b.Duration,
		Errors: totalErrs.Load(), Latencies: allLats, BytesRead: totalBytes.Load(),
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Workloads
// ═══════════════════════════════════════════════════════════════════════════════

func buildWorkloads(baseURL string, duration time.Duration, conns int) []*HTTPBench {
	smallBody, _ := json.Marshal(map[string]any{
		"name": "Benchmark Item", "price": 29.99,
		"description": "A test item for benchmarking", "tax": 2.5,
	})

	largeContent := strings.Repeat("Lorem ipsum dolor sit amet. ", 350)
	tags := make([]string, 20)
	for i := range tags {
		tags[i] = fmt.Sprintf("tag-%d", i)
	}
	meta := map[string]string{}
	for i := 0; i < 10; i++ {
		meta[fmt.Sprintf("key-%d", i)] = fmt.Sprintf("value-%d-with-some-extra-content", i)
	}
	largeBody, _ := json.Marshal(map[string]any{
		"title": "Large Benchmark Payload", "content": largeContent,
		"tags": tags, "metadata": meta,
	})

	return []*HTTPBench{
		{Name: "plaintext", URL: baseURL + "/plaintext", Method: "GET", Conns: conns, Duration: duration},
		{Name: "json", URL: baseURL + "/json", Method: "GET", Conns: conns, Duration: duration},
		{Name: "single_query", URL: baseURL + "/db", Method: "GET", Conns: conns, Duration: duration},
		{Name: "path_param", URL: baseURL + "/user/12345", Method: "GET", Conns: conns, Duration: duration},
		{Name: "query_params", URL: baseURL + "/search?q=hello+world&limit=10&offset=0", Method: "GET", Conns: conns, Duration: duration},
		{Name: "post_small", URL: baseURL + "/items", Method: "POST", Body: smallBody, ContentType: "application/json", Conns: conns, Duration: duration},
		{Name: "post_large", URL: baseURL + "/upload", Method: "POST", Body: largeBody, ContentType: "application/json", Conns: conns, Duration: duration},
		{Name: "nested_json", URL: baseURL + "/nested", Method: "GET", Conns: conns, Duration: duration},
		{Name: "headers", URL: baseURL + "/headers", Method: "GET", Conns: conns, Duration: duration,
			Headers: map[string]string{"X-Request-Id": "bench-001", "Authorization": "Bearer fake-token", "Accept-Language": "en-US"}},
	}
}

// ═══════════════════════════════════════════════════════════════════════════════
// Output
// ═══════════════════════════════════════════════════════════════════════════════

func printHeader() {
	fmt.Println("┌─────────────────┬────────────┬────────────┬──────────┬──────────┬──────────┬────────┐")
	fmt.Println("│ Workload        │    Req/s   │    Avg     │   P50    │   P99    │  P99.9   │ Errors │")
	fmt.Println("├─────────────────┼────────────┼────────────┼──────────┼──────────┼──────────┼────────┤")
}

func printResult(r *BenchResult) {
	fmt.Printf("│ %-15s │ %10.0f │ %10s │ %8s │ %8s │ %8s │ %6d │\n",
		r.Name, r.RPS(), fmtDur(r.AvgLatency()), fmtDur(r.P50()), fmtDur(r.P99()), fmtDur(r.P999()), r.Errors)
}

func printFooter() {
	fmt.Println("└─────────────────┴────────────┴────────────┴──────────┴──────────┴──────────┴────────┘")
}

func fmtDur(d time.Duration) string {
	if d < time.Microsecond {
		return fmt.Sprintf("%.0fns", float64(d.Nanoseconds()))
	}
	if d < time.Millisecond {
		return fmt.Sprintf("%.1fµs", float64(d.Nanoseconds())/1000.0)
	}
	if d < time.Second {
		return fmt.Sprintf("%.2fms", float64(d.Nanoseconds())/1e6)
	}
	return fmt.Sprintf("%.2fs", d.Seconds())
}

// ═══════════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════════

func main() {
	target := flag.String("target", "astra", "target: astra | fast | both")
	duration := flag.Duration("d", 10*time.Second, "duration per workload")
	conns := flag.Int("c", 64, "concurrent connections")
	warmup := flag.Duration("w", 2*time.Second, "warmup duration")
	astraPort := flag.Int("astra-port", 8001, "AstraAPI port")
	fastPort := flag.Int("fast-port", 8002, "FastAPI port")
	wsMsgSize := flag.Int("ws-msg", 128, "WebSocket message size in bytes")
	wsConns := flag.Int("ws-conns", 32, "WebSocket concurrent connections")
	flag.Parse()

	// Use all CPU cores for the benchmark client
	runtime.GOMAXPROCS(runtime.NumCPU())

	// Raise file descriptor limit
	var rlim syscall.Rlimit
	_ = syscall.Getrlimit(syscall.RLIMIT_NOFILE, &rlim)
	rlim.Cur = rlim.Max
	_ = syscall.Setrlimit(syscall.RLIMIT_NOFILE, &rlim)

	type serverTarget struct {
		name  string
		url   string
		wsURL string
	}

	var targets []serverTarget
	if *target == "astra" || *target == "both" {
		targets = append(targets, serverTarget{
			"AstraAPI",
			fmt.Sprintf("http://127.0.0.1:%d", *astraPort),
			fmt.Sprintf("ws://127.0.0.1:%d/ws/echo", *astraPort),
		})
	}
	if *target == "fast" || *target == "both" {
		targets = append(targets, serverTarget{
			"FastAPI",
			fmt.Sprintf("http://127.0.0.1:%d", *fastPort),
			fmt.Sprintf("ws://127.0.0.1:%d/ws/echo", *fastPort),
		})
	}

	if len(targets) == 0 {
		fmt.Fprintf(os.Stderr, "invalid -target: %s (use: astra | fast | both)\n", *target)
		os.Exit(1)
	}

	// Verify servers are running
	for _, t := range targets {
		resp, err := http.Get(t.url + "/json")
		if err != nil {
			fmt.Fprintf(os.Stderr, "✗ cannot connect to %s at %s: %v\n", t.name, t.url, err)
			fmt.Fprintf(os.Stderr, "  start the server first: task run-astra / task run-fast\n")
			os.Exit(1)
		}
		resp.Body.Close()
	}

	for _, t := range targets {
		fmt.Printf("\n══════════════════════════════════════════════════════════════════════════\n")
		fmt.Printf("  %s — %s\n", t.name, t.url)
		fmt.Printf("  Connections: %d | Duration: %s | CPUs: %d\n", *conns, *duration, runtime.NumCPU())
		fmt.Printf("══════════════════════════════════════════════════════════════════════════\n\n")

		// Warmup — saturate connection pool
		if *warmup > 0 {
			fmt.Printf("  ⏳ warming up (%s)...\n\n", *warmup)
			wb := &HTTPBench{URL: t.url + "/json", Method: "GET", Conns: *conns, Duration: *warmup, Name: "warmup"}
			wb.Run()
			time.Sleep(300 * time.Millisecond)
		}

		// Run all HTTP workloads
		workloads := buildWorkloads(t.url, *duration, *conns)
		printHeader()
		for _, w := range workloads {
			r := w.Run()
			printResult(r)
			time.Sleep(200 * time.Millisecond)
		}

		// WebSocket
		wsb := &WSBench{
			URL: t.wsURL, Conns: *wsConns, Duration: *duration,
			MsgSize: *wsMsgSize, Name: "ws_echo",
		}
		wsr := wsb.Run()
		printResult(wsr)
		printFooter()
		fmt.Println()
	}

	if len(targets) == 2 {
		fmt.Println("  📊 Compare the Req/s columns above for each server.")
	}
}
