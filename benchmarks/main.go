package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

// ── Configuration ───────────────────────────────────────────────────────────

const (
	TestDuration = 15 * time.Second
	WarmupTime   = 3 * time.Second
	Concurrency  = 100
	NumRuns      = 3
)

// ── Types ───────────────────────────────────────────────────────────────────

type Stats struct {
	TotalRequests int     `json:"total_requests"`
	RPS           float64 `json:"requests_per_second"`
	P50           float64 `json:"latency_p50_ms"`
	P95           float64 `json:"latency_p95_ms"`
	P99           float64 `json:"latency_p99_ms"`
	Avg           float64 `json:"latency_avg_ms"`
	Min           float64 `json:"latency_min_ms"`
	Max           float64 `json:"latency_max_ms"`
	SuccessRate   float64 `json:"success_rate"`
}

type BenchResult struct {
	TestName       string  `json:"test_name"`
	Implementation string  `json:"implementation"`
	Duration       float64 `json:"duration_seconds"`
	Stats
}

type TestCase struct {
	Name        string
	Endpoint    string
	Method      string
	Body        []byte
	Headers     map[string]string
	Concurrency int
}

// ── WebSocket Test Types ────────────────────────────────────────────────────

type WSTestCase struct {
	Name        string
	Endpoint    string
	Mode        string // "echo", "json_echo", "throughput"
	MsgPayload  string
	MsgCount    int // messages per connection (echo modes)
	Concurrency int
}

type WSStats struct {
	TotalMessages int     `json:"total_messages"`
	MPS           float64 `json:"messages_per_second"`
	P50           float64 `json:"latency_p50_ms"`
	P95           float64 `json:"latency_p95_ms"`
	P99           float64 `json:"latency_p99_ms"`
	Avg           float64 `json:"latency_avg_ms"`
	Min           float64 `json:"latency_min_ms"`
	Max           float64 `json:"latency_max_ms"`
	SuccessRate   float64 `json:"success_rate"`
	ConnErrors    int     `json:"connection_errors"`
}

type WSBenchResult struct {
	TestName       string  `json:"test_name"`
	Implementation string  `json:"implementation"`
	Duration       float64 `json:"duration_seconds"`
	WSStats
}

// ── HTTP Load Generator ─────────────────────────────────────────────────────

func fireRequests(url, method string, body []byte, headers map[string]string,
	duration, warmup time.Duration, concurrency int) (latencies []float64, ok, fail int) {

	transport := &http.Transport{
		MaxIdleConns:        concurrency * 2,
		MaxIdleConnsPerHost: concurrency * 2,
		MaxConnsPerHost:     concurrency * 2,
		IdleConnTimeout:     90 * time.Second,
		DisableCompression:  true,
		DialContext: (&net.Dialer{
			Timeout:   5 * time.Second,
			KeepAlive: 30 * time.Second,
		}).DialContext,
	}
	client := &http.Client{Transport: transport, Timeout: 30 * time.Second}
	defer transport.CloseIdleConnections()

	var (
		mu       sync.Mutex
		okCount  int64
		errCount int64
	)

	start := time.Now()
	warmupEnd := start.Add(warmup)
	testEnd := warmupEnd.Add(duration)

	var wg sync.WaitGroup
	wg.Add(concurrency)

	for i := 0; i < concurrency; i++ {
		go func() {
			defer wg.Done()
			local := make([]float64, 0, 8192)

			for time.Now().Before(testEnd) {
				var req *http.Request
				var err error

				if body != nil {
					req, err = http.NewRequest(method, url, bytes.NewReader(body))
				} else {
					req, err = http.NewRequest(method, url, nil)
				}
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errCount, 1)
					}
					continue
				}
				for k, v := range headers {
					req.Header.Set(k, v)
				}

				t0 := time.Now()
				resp, err := client.Do(req)
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errCount, 1)
					}
					continue
				}
				io.Copy(io.Discard, resp.Body)
				resp.Body.Close()
				elapsed := float64(time.Since(t0).Microseconds()) / 1000.0

				if time.Now().After(warmupEnd) {
					if resp.StatusCode >= 200 && resp.StatusCode < 300 {
						local = append(local, elapsed)
						atomic.AddInt64(&okCount, 1)
					} else {
						atomic.AddInt64(&errCount, 1)
					}
				}
			}

			mu.Lock()
			latencies = append(latencies, local...)
			mu.Unlock()
		}()
	}

	wg.Wait()
	return latencies, int(okCount), int(errCount)
}

func computeStats(lats []float64, durationSec float64, ok, fail int) Stats {
	if len(lats) == 0 {
		return Stats{}
	}
	sort.Float64s(lats)
	n := len(lats)

	var sum float64
	for _, v := range lats {
		sum += v
	}

	total := ok + fail
	sr := 0.0
	if total > 0 {
		sr = float64(ok) / float64(total) * 100
	}

	return Stats{
		TotalRequests: n,
		RPS:           math.Round(float64(n)/durationSec*10) / 10,
		P50:           lats[n*50/100],
		P95:           lats[intMin(n*95/100, n-1)],
		P99:           lats[intMin(n*99/100, n-1)],
		Avg:           math.Round(sum/float64(n)*100) / 100,
		Min:           lats[0],
		Max:           lats[n-1],
		SuccessRate:   math.Round(sr*10) / 10,
	}
}

// Run multiple rounds and combine all latencies for stable results
func benchmark(url, method string, body []byte, headers map[string]string, concurrency int) Stats {
	var allLats []float64
	var totalOk, totalFail int

	for run := 0; run < NumRuns; run++ {
		lats, ok, fail := fireRequests(url, method, body, headers, TestDuration, WarmupTime, concurrency)
		allLats = append(allLats, lats...)
		totalOk += ok
		totalFail += fail
	}

	return computeStats(allLats, TestDuration.Seconds()*float64(NumRuns), totalOk, totalFail)
}

// ── Server Management ───────────────────────────────────────────────────────

type Server struct {
	cmd  *exec.Cmd
	port int
}

// startUvicornServer starts a Python app via uvicorn (for FastAPI baseline)
func startUvicornServer(pythonExe, appModule string, port int) *Server {
	cmd := exec.Command(pythonExe, "-m", "uvicorn",
		appModule,
		"--host", "127.0.0.1",
		"--port", fmt.Sprintf("%d", port),
		"--log-level", "error",
		"--no-access-log",
	)
	cmd.Stdout = nil
	cmd.Stderr = nil

	if err := cmd.Start(); err != nil {
		fmt.Printf("    ERROR: failed to start %s: %v\n", appModule, err)
		return &Server{port: port}
	}

	return waitForServer(cmd, port)
}

func waitForServer(cmd *exec.Cmd, port int) *Server {
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), 200*time.Millisecond)
		if err == nil {
			conn.Close()
			time.Sleep(500 * time.Millisecond) // let server fully warm up
			return &Server{cmd: cmd, port: port}
		}
		time.Sleep(200 * time.Millisecond)
	}

	fmt.Printf("    WARNING: server on port %d may not be ready\n", port)
	return &Server{cmd: cmd, port: port}
}

func (s *Server) Stop() {
	if s.cmd == nil || s.cmd.Process == nil {
		return
	}
	s.cmd.Process.Signal(syscall.SIGTERM)
	done := make(chan error, 1)
	go func() { done <- s.cmd.Wait() }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		s.cmd.Process.Kill()
		<-done
	}
}

// ── WebSocket Load Generator ────────────────────────────────────────────────

// wsEchoBench: each goroutine opens a WS, sends msgCount messages, measures RTT
func wsEchoBench(wsURL string, payload string, msgCount, concurrency int,
	duration, warmup time.Duration) (latencies []float64, okCount, errCount int) {

	var (
		mu       sync.Mutex
		okTotal  int64
		errTotal int64
	)

	start := time.Now()
	warmupEnd := start.Add(warmup)
	testEnd := warmupEnd.Add(duration)

	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
	}

	var wg sync.WaitGroup
	wg.Add(concurrency)

	for i := 0; i < concurrency; i++ {
		go func() {
			defer wg.Done()
			local := make([]float64, 0, 4096)

			for time.Now().Before(testEnd) {
				conn, _, err := dialer.Dial(wsURL, nil)
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errTotal, 1)
					}
					time.Sleep(10 * time.Millisecond)
					continue
				}

				for m := 0; m < msgCount && time.Now().Before(testEnd); m++ {
					t0 := time.Now()
					err := conn.WriteMessage(websocket.TextMessage, []byte(payload))
					if err != nil {
						if time.Now().After(warmupEnd) {
							atomic.AddInt64(&errTotal, 1)
						}
						break
					}
					_, _, err = conn.ReadMessage()
					if err != nil {
						if time.Now().After(warmupEnd) {
							atomic.AddInt64(&errTotal, 1)
						}
						break
					}
					elapsed := float64(time.Since(t0).Microseconds()) / 1000.0

					if time.Now().After(warmupEnd) {
						local = append(local, elapsed)
						atomic.AddInt64(&okTotal, 1)
					}
				}
				conn.Close()
			}

			mu.Lock()
			latencies = append(latencies, local...)
			mu.Unlock()
		}()
	}

	wg.Wait()
	return latencies, int(okTotal), int(errTotal)
}

// wsJSONEchoBench: same as echo but sends/receives JSON
func wsJSONEchoBench(wsURL string, jsonPayload string, msgCount, concurrency int,
	duration, warmup time.Duration) (latencies []float64, okCount, errCount int) {

	var (
		mu       sync.Mutex
		okTotal  int64
		errTotal int64
	)

	start := time.Now()
	warmupEnd := start.Add(warmup)
	testEnd := warmupEnd.Add(duration)

	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
	}

	var wg sync.WaitGroup
	wg.Add(concurrency)

	for i := 0; i < concurrency; i++ {
		go func() {
			defer wg.Done()
			local := make([]float64, 0, 4096)

			for time.Now().Before(testEnd) {
				conn, _, err := dialer.Dial(wsURL, nil)
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errTotal, 1)
					}
					time.Sleep(10 * time.Millisecond)
					continue
				}

				for m := 0; m < msgCount && time.Now().Before(testEnd); m++ {
					t0 := time.Now()
					err := conn.WriteMessage(websocket.TextMessage, []byte(jsonPayload))
					if err != nil {
						if time.Now().After(warmupEnd) {
							atomic.AddInt64(&errTotal, 1)
						}
						break
					}
					_, _, err = conn.ReadMessage()
					if err != nil {
						if time.Now().After(warmupEnd) {
							atomic.AddInt64(&errTotal, 1)
						}
						break
					}
					elapsed := float64(time.Since(t0).Microseconds()) / 1000.0

					if time.Now().After(warmupEnd) {
						local = append(local, elapsed)
						atomic.AddInt64(&okTotal, 1)
					}
				}
				conn.Close()
			}

			mu.Lock()
			latencies = append(latencies, local...)
			mu.Unlock()
		}()
	}

	wg.Wait()
	return latencies, int(okTotal), int(errTotal)
}

// wsThroughputBench: connect, send count, receive that many messages, measure total time
func wsThroughputBench(wsURL string, receiveCount, concurrency int,
	duration, warmup time.Duration) (latencies []float64, okCount, errCount int) {

	var (
		mu       sync.Mutex
		okTotal  int64
		errTotal int64
	)

	start := time.Now()
	warmupEnd := start.Add(warmup)
	testEnd := warmupEnd.Add(duration)

	dialer := websocket.Dialer{
		HandshakeTimeout: 5 * time.Second,
	}

	countStr := fmt.Sprintf("%d", receiveCount)

	var wg sync.WaitGroup
	wg.Add(concurrency)

	for i := 0; i < concurrency; i++ {
		go func() {
			defer wg.Done()
			local := make([]float64, 0, 2048)

			for time.Now().Before(testEnd) {
				conn, _, err := dialer.Dial(wsURL, nil)
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errTotal, 1)
					}
					time.Sleep(10 * time.Millisecond)
					continue
				}

				t0 := time.Now()
				err = conn.WriteMessage(websocket.TextMessage, []byte(countStr))
				if err != nil {
					if time.Now().After(warmupEnd) {
						atomic.AddInt64(&errTotal, 1)
					}
					conn.Close()
					continue
				}

				allOK := true
				for r := 0; r < receiveCount; r++ {
					_, _, err = conn.ReadMessage()
					if err != nil {
						allOK = false
						break
					}
				}

				elapsed := float64(time.Since(t0).Microseconds()) / 1000.0

				if time.Now().After(warmupEnd) {
					if allOK {
						local = append(local, elapsed)
						atomic.AddInt64(&okTotal, 1)
					} else {
						atomic.AddInt64(&errTotal, 1)
					}
				}
				conn.Close()
			}

			mu.Lock()
			latencies = append(latencies, local...)
			mu.Unlock()
		}()
	}

	wg.Wait()
	return latencies, int(okTotal), int(errTotal)
}

func computeWSStats(lats []float64, durationSec float64, ok, fail int) WSStats {
	if len(lats) == 0 {
		return WSStats{ConnErrors: fail}
	}
	sort.Float64s(lats)
	n := len(lats)

	var sum float64
	for _, v := range lats {
		sum += v
	}

	total := ok + fail
	sr := 0.0
	if total > 0 {
		sr = float64(ok) / float64(total) * 100
	}

	return WSStats{
		TotalMessages: n,
		MPS:           math.Round(float64(n)/durationSec*10) / 10,
		P50:           lats[n*50/100],
		P95:           lats[intMin(n*95/100, n-1)],
		P99:           lats[intMin(n*99/100, n-1)],
		Avg:           math.Round(sum/float64(n)*100) / 100,
		Min:           lats[0],
		Max:           lats[n-1],
		SuccessRate:   math.Round(sr*10) / 10,
		ConnErrors:    fail,
	}
}

func benchmarkWS(tc WSTestCase, host string, port int) WSStats {
	u := url.URL{Scheme: "ws", Host: fmt.Sprintf("%s:%d", host, port), Path: tc.Endpoint}
	wsURL := u.String()

	var allLats []float64
	var totalOk, totalFail int

	for run := 0; run < NumRuns; run++ {
		var lats []float64
		var ok, fail int

		switch tc.Mode {
		case "echo":
			lats, ok, fail = wsEchoBench(wsURL, tc.MsgPayload, tc.MsgCount, tc.Concurrency, TestDuration, WarmupTime)
		case "json_echo":
			lats, ok, fail = wsJSONEchoBench(wsURL, tc.MsgPayload, tc.MsgCount, tc.Concurrency, TestDuration, WarmupTime)
		case "throughput":
			lats, ok, fail = wsThroughputBench(wsURL, tc.MsgCount, tc.Concurrency, TestDuration, WarmupTime)
		}

		allLats = append(allLats, lats...)
		totalOk += ok
		totalFail += fail
	}

	return computeWSStats(allLats, TestDuration.Seconds()*float64(NumRuns), totalOk, totalFail)
}

// ── Output ──────────────────────────────────────────────────────────────────

func printComparison(r, p BenchResult) {
	if p.RPS == 0 {
		return
	}
	rpsPct := (r.RPS/p.RPS - 1) * 100
	latPct := 0.0
	if r.P50 > 0 {
		latPct = (p.P50/r.P50 - 1) * 100
	}

	winner := "C++"
	color := "\033[32m"
	if rpsPct < 0 {
		winner = "FASTAPI"
		color = "\033[31m"
	}
	reset := "\033[0m"

	fmt.Printf("\n  %s%s wins: %+.1f%% RPS, %+.1f%% faster p50%s\n",
		color, winner, rpsPct, latPct, reset)
	fmt.Printf("    RPS:  C++: %8.0f  FastAPI: %8.0f\n", r.RPS, p.RPS)
	fmt.Printf("    p50:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P50, p.P50)
	fmt.Printf("    p95:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P95, p.P95)
	fmt.Printf("    p99:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P99, p.P99)
}

func printSummary(results []BenchResult) {
	fmt.Printf("\n%s\n", strings.Repeat("=", 80))
	fmt.Println("  FINAL SUMMARY")
	fmt.Println(strings.Repeat("=", 80))

	var cppRPS, pyRPS, cppLat, pyLat float64
	var cppN, pyN int

	for _, r := range results {
		switch r.Implementation {
		case "cpp":
			cppRPS += r.RPS
			cppLat += r.P50
			cppN++
		default:
			pyRPS += r.RPS
			pyLat += r.P50
			pyN++
		}
	}

	if cppN == 0 || pyN == 0 {
		return
	}

	avgCpp := cppRPS / float64(cppN)
	avgPy := pyRPS / float64(pyN)
	pct := (avgCpp/avgPy - 1) * 100

	avgCppLat := cppLat / float64(cppN)
	avgPyLat := pyLat / float64(pyN)
	latPct := 0.0
	if avgCppLat > 0 {
		latPct = (avgPyLat/avgCppLat - 1) * 100
	}

	color := "\033[32m"
	if pct < 0 {
		color = "\033[31m"
	}
	reset := "\033[0m"

	fmt.Printf("\n  %sAverage RPS:  C++: %8.0f  FastAPI: %8.0f  (%+.1f%%)%s\n",
		color, avgCpp, avgPy, pct, reset)
	fmt.Printf("  Average p50:  C++: %7.2fms  FastAPI: %7.2fms  (%+.1f%%)\n",
		avgCppLat, avgPyLat, latPct)

	fmt.Printf("\n  %-25s %10s %10s %10s\n", "Test", "C++ RPS", "FA RPS", "Delta")
	fmt.Println("  " + strings.Repeat("-", 58))
	for i := 0; i+1 < len(results); i += 2 {
		r := results[i]
		p := results[i+1]
		if p.RPS == 0 {
			continue
		}
		d := (r.RPS/p.RPS - 1) * 100
		fmt.Printf("  %-25s %10.0f %10.0f %+9.1f%%\n", r.TestName, r.RPS, p.RPS, d)
	}

	fmt.Println()
	fmt.Println(strings.Repeat("=", 80))
}

func printWSComparison(r, p WSBenchResult) {
	if p.MPS == 0 {
		return
	}
	mpsPct := (r.MPS/p.MPS - 1) * 100
	latPct := 0.0
	if r.P50 > 0 {
		latPct = (p.P50/r.P50 - 1) * 100
	}

	winner := "C++"
	color := "\033[32m"
	if mpsPct < 0 {
		winner = "FASTAPI"
		color = "\033[31m"
	}
	reset := "\033[0m"

	fmt.Printf("\n  %s%s wins: %+.1f%% MPS, %+.1f%% faster p50%s\n",
		color, winner, mpsPct, latPct, reset)
	fmt.Printf("    MPS:  C++: %8.0f  FastAPI: %8.0f\n", r.MPS, p.MPS)
	fmt.Printf("    p50:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P50, p.P50)
	fmt.Printf("    p95:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P95, p.P95)
	fmt.Printf("    p99:  C++: %7.2fms  FastAPI: %7.2fms\n", r.P99, p.P99)
	fmt.Printf("    Err:  C++: %8d  FastAPI: %8d\n", r.ConnErrors, p.ConnErrors)
}

func printWSSummary(results []WSBenchResult) {
	fmt.Printf("\n%s\n", strings.Repeat("=", 80))
	fmt.Println("  WEBSOCKET BENCHMARK SUMMARY")
	fmt.Println(strings.Repeat("=", 80))

	var cppMPS, pyMPS, cppLat, pyLat float64
	var cppN, pyN int

	for _, r := range results {
		switch r.Implementation {
		case "cpp":
			cppMPS += r.MPS
			cppLat += r.P50
			cppN++
		default:
			pyMPS += r.MPS
			pyLat += r.P50
			pyN++
		}
	}

	if cppN == 0 || pyN == 0 {
		return
	}

	avgCpp := cppMPS / float64(cppN)
	avgPy := pyMPS / float64(pyN)
	pct := (avgCpp/avgPy - 1) * 100

	avgCppLat := cppLat / float64(cppN)
	avgPyLat := pyLat / float64(pyN)
	latPct := 0.0
	if avgCppLat > 0 {
		latPct = (avgPyLat/avgCppLat - 1) * 100
	}

	color := "\033[32m"
	if pct < 0 {
		color = "\033[31m"
	}
	reset := "\033[0m"

	fmt.Printf("\n  %sAverage MPS:  C++: %8.0f  FastAPI: %8.0f  (%+.1f%%)%s\n",
		color, avgCpp, avgPy, pct, reset)
	fmt.Printf("  Average p50:  C++: %7.2fms  FastAPI: %7.2fms  (%+.1f%%)\n",
		avgCppLat, avgPyLat, latPct)

	fmt.Printf("\n  %-30s %10s %10s %10s\n", "Test", "C++ MPS", "FA MPS", "Delta")
	fmt.Println("  " + strings.Repeat("-", 63))
	for i := 0; i+1 < len(results); i += 2 {
		r := results[i]
		p := results[i+1]
		if p.MPS == 0 {
			continue
		}
		d := (r.MPS/p.MPS - 1) * 100
		fmt.Printf("  %-30s %10.0f %10.0f %+9.1f%%\n", r.TestName, r.MPS, p.MPS, d)
	}

	fmt.Println()
	fmt.Println(strings.Repeat("=", 80))
}

func saveWSResults(results []WSBenchResult) {
	data, err := json.MarshalIndent(results, "", "  ")
	if err != nil {
		return
	}
	os.MkdirAll("benchmarks", 0755)
	os.WriteFile("benchmarks/ws_results.json", data, 0644)
	fmt.Println("\n  Results saved to benchmarks/ws_results.json")
}

func saveResults(results []BenchResult) {
	data, err := json.MarshalIndent(results, "", "  ")
	if err != nil {
		return
	}
	os.MkdirAll("benchmarks", 0755)
	os.WriteFile("benchmarks/results.json", data, 0644)
	fmt.Println("\n  Results saved to benchmarks/results.json")
}

// ── Main ────────────────────────────────────────────────────────────────────

func main() {
	pythonExe := findPython()
	if pythonExe == "" {
		fmt.Println("ERROR: python3/python not found in PATH")
		os.Exit(1)
	}

	fmt.Println(strings.Repeat("=", 80))
	fmt.Println("  FastAPI + C++ Core vs FastAPI Pure Python — WebSocket Benchmark")
	fmt.Printf("  Go %s | %d goroutines | %v x %d runs | %v warmup\n",
		runtime.Version(), Concurrency, TestDuration, NumRuns, WarmupTime)
	fmt.Println(strings.Repeat("=", 80))

	// ── REST API tests (commented out — uncomment to re-enable) ────────────
	//
	// tests := []TestCase{
	// 	{Name: "Simple GET", Endpoint: "/simple", Method: "GET", Concurrency: Concurrency},
	// 	{Name: "Path Parameter", Endpoint: "/items/123", Method: "GET", Concurrency: Concurrency},
	// 	{Name: "Query Parameters", Endpoint: "/search?q=test&page=1&limit=20&sort=name", Method: "GET", Concurrency: Concurrency},
	// 	{
	// 		Name: "POST JSON (small)", Endpoint: "/items", Method: "POST",
	// 		Body:        []byte(`{"name":"Test","price":99.99,"quantity":5,"tags":["t"]}`),
	// 		Headers:     map[string]string{"Content-Type": "application/json"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "POST JSON (medium)", Endpoint: "/users", Method: "POST",
	// 		Body:        []byte(`{"username":"testuser","email":"test@example.com","age":30}`),
	// 		Headers:     map[string]string{"Content-Type": "application/json"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{Name: "Large Response (100)", Endpoint: "/large?count=100", Method: "GET", Concurrency: Concurrency},
	// 	{
	// 		Name: "Headers", Endpoint: "/headers", Method: "GET",
	// 		Headers:     map[string]string{"Authorization": "Bearer test-token-12345", "Accept": "application/json", "X-Request-ID": "bench-001"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{Name: "High Concurrency", Endpoint: "/simple", Method: "GET", Concurrency: 500},
	// 	{
	// 		Name: "POST Raw JSON", Endpoint: "/post_raw", Method: "POST",
	// 		Body:        []byte(`{"key":"value","count":42,"active":true}`),
	// 		Headers:     map[string]string{"Content-Type": "application/json"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "POST Large JSON", Endpoint: "/post_large", Method: "POST",
	// 		Body:        []byte(`{"user_id":123,"username":"testuser","email":"test@example.com","first_name":"John","last_name":"Doe","age":30,"address":{"street":"123 Main St","city":"NYC","zip":"10001"},"metadata":{"source":"api","version":"2.0"},"tags":["admin","active","premium"],"is_active":true,"created_at":"2024-01-01T00:00:00Z","updated_at":"2024-06-15T12:30:00Z"}`),
	// 		Headers:     map[string]string{"Content-Type": "application/json"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "CORS Preflight", Endpoint: "/simple", Method: "OPTIONS",
	// 		Headers:     map[string]string{"Origin": "http://localhost:3000", "Access-Control-Request-Method": "POST", "Access-Control-Request-Headers": "content-type"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "Mixed Params", Endpoint: "/mixed/42?q=test&limit=20", Method: "GET",
	// 		Headers:     map[string]string{"Authorization": "Bearer token123", "Cookie": "session_id=abc123"},
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "Dependency Injection", Endpoint: "/with_deps?q=search&skip=10", Method: "GET",
	// 		Concurrency: Concurrency,
	// 	},
	// 	{
	// 		Name: "Response Model", Endpoint: "/user/123", Method: "GET",
	// 		Concurrency: Concurrency,
	// 	},
	// }
	//
	// var allResults []BenchResult
	//
	// for _, tc := range tests {
	// 	fmt.Printf("\n%s\n", tc.Name)
	// 	fmt.Println(strings.Repeat("-", 80))
	//
	// 	hdrs := tc.Headers
	// 	if hdrs == nil {
	// 		hdrs = map[string]string{}
	// 	}
	// 	conc := tc.Concurrency
	//
	// 	// ── C++ FastAPI (built-in HTTP server) ──────────
	// 	rStats := benchmark(
	// 		fmt.Sprintf("http://127.0.0.1:8002%s", tc.Endpoint),
	// 		tc.Method, tc.Body, hdrs, conc,
	// 	)
	// 	rr := BenchResult{TestName: tc.Name, Implementation: "cpp",
	// 		Duration: TestDuration.Seconds() * float64(NumRuns), Stats: rStats}
	// 	allResults = append(allResults, rr)
	// 	fmt.Printf("  %-25s C++:       %7.0f req/s  p50: %6.2fms\n", tc.Name, rStats.RPS, rStats.P50)
	// 	time.Sleep(time.Second)
	//
	// 	// ── Pure Python FastAPI (uvicorn) ────────────────
	// 	ps := startUvicornServer(pythonExe, "benchmarks.normal.main:app", 8001)
	// 	pStats := benchmark(
	// 		fmt.Sprintf("http://127.0.0.1:8001%s", tc.Endpoint),
	// 		tc.Method, tc.Body, hdrs, conc,
	// 	)
	// 	pr := BenchResult{TestName: tc.Name, Implementation: "fastapi",
	// 		Duration: TestDuration.Seconds() * float64(NumRuns), Stats: pStats}
	// 	allResults = append(allResults, pr)
	// 	fmt.Printf("  %-25s FastAPI:   %7.0f req/s  p50: %6.2fms\n", tc.Name, pStats.RPS, pStats.P50)
	// 	ps.Stop()
	// 	time.Sleep(time.Second)
	//
	// 	printComparison(rr, pr)
	// }
	//
	// saveResults(allResults)
	// printSummary(allResults)

	// ── WebSocket benchmark tests ──────────────────────────────────────────

	smallJSON := `{"key":"value","count":42,"active":true}`
	mediumJSON := `{"user_id":123,"username":"testuser","email":"test@example.com","first_name":"John","last_name":"Doe","age":30,"tags":["admin","active","premium"],"is_active":true}`

	wsTests := []WSTestCase{
		{
			Name:        "WS Echo (small)",
			Endpoint:    "/ws_echo",
			Mode:        "echo",
			MsgPayload:  "Hello, WebSocket!",
			MsgCount:    500,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS Echo (medium)",
			Endpoint:    "/ws_echo",
			Mode:        "echo",
			MsgPayload:  strings.Repeat("x", 256),
			MsgCount:    500,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS Echo (large 4KB)",
			Endpoint:    "/ws_echo",
			Mode:        "echo",
			MsgPayload:  strings.Repeat("A", 4096),
			MsgCount:    200,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS JSON Echo (small)",
			Endpoint:    "/ws_json",
			Mode:        "json_echo",
			MsgPayload:  smallJSON,
			MsgCount:    500,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS JSON Echo (medium)",
			Endpoint:    "/ws_json",
			Mode:        "json_echo",
			MsgPayload:  mediumJSON,
			MsgCount:    500,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS Throughput (100)",
			Endpoint:    "/ws_throughput",
			Mode:        "throughput",
			MsgCount:    100,
			Concurrency: Concurrency,
		},
		{
			Name:        "WS Throughput (1000)",
			Endpoint:    "/ws_throughput",
			Mode:        "throughput",
			MsgCount:    1000,
			Concurrency: 50,
		},
		{
			Name:        "WS Echo High Conc",
			Endpoint:    "/ws_echo",
			Mode:        "echo",
			MsgPayload:  "ping",
			MsgCount:    200,
			Concurrency: 500,
		},
	}

	var wsResults []WSBenchResult

	// Graceful shutdown on Ctrl+C
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigCh
		fmt.Println("\n  Interrupted")
		os.Exit(1)
	}()

	for _, tc := range wsTests {
		fmt.Printf("\n%s\n", tc.Name)
		fmt.Println(strings.Repeat("-", 80))

		// ── C++ FastAPI (built-in WS server on port 8002) ──────────
		cppStats := benchmarkWS(tc, "127.0.0.1", 8002)
		cppR := WSBenchResult{TestName: tc.Name, Implementation: "cpp",
			Duration: TestDuration.Seconds() * float64(NumRuns), WSStats: cppStats}
		wsResults = append(wsResults, cppR)
		fmt.Printf("  %-30s C++:       %7.0f msg/s  p50: %6.2fms  err: %d\n",
			tc.Name, cppStats.MPS, cppStats.P50, cppStats.ConnErrors)
		time.Sleep(time.Second)

		// ── Pure Python FastAPI (uvicorn on port 8001) ────────────
		// ps := startUvicornServer(pythonExe, "benchmarks.normal.main:app", 8001)
		pyStats := benchmarkWS(tc, "127.0.0.1", 8001)
		pyR := WSBenchResult{TestName: tc.Name, Implementation: "fastapi",
			Duration: TestDuration.Seconds() * float64(NumRuns), WSStats: pyStats}
		wsResults = append(wsResults, pyR)
		fmt.Printf("  %-30s FastAPI:   %7.0f msg/s  p50: %6.2fms  err: %d\n",
			tc.Name, pyStats.MPS, pyStats.P50, pyStats.ConnErrors)
		// ps.Stop()
		time.Sleep(time.Second)

		printWSComparison(cppR, pyR)
	}

	saveWSResults(wsResults)
	printWSSummary(wsResults)
}

func findPython() string {
	for _, name := range []string{"python3", "python"} {
		if p, err := exec.LookPath(name); err == nil {
			return p
		}
	}
	return ""
}

func intMin(a, b int) int {
	if a < b {
		return a
	}
	return b
}
