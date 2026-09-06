import urllib.request
import time
import ssl
import sys

def test_stream(url, duration_sec=35, chunk_size=2048):
    print(f"\n==================================================")
    print(f"Testing URL: {url}")
    print(f"Duration: {duration_sec} seconds, Chunk size: {chunk_size} bytes")
    print(f"==================================================")

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    headers = {
        'User-Agent': 'xiaozhi-esp32-radio/1.0',
        'Icy-MetaData': '0'
    }

    req = urllib.request.Request(url, headers=headers)

    try:
        start_connect = time.perf_counter()
        resp = urllib.request.urlopen(req, context=ctx, timeout=10)
        connect_time = (time.perf_counter() - start_connect) * 1000.0

        final_url = resp.geturl()
        status_code = resp.getcode()
        content_type = resp.headers.get('Content-Type', 'unknown')

        print(f"Connected in {connect_time:.1f} ms")
        print(f"HTTP Status:  {status_code}")
        print(f"Content-Type: {content_type}")
        print(f"Final URL:    {final_url}")
        print("Reading stream data...\n")

        total_bytes = 0
        read_count = 0
        intervals = []
        gaps_250 = 0
        gaps_500 = 0
        gaps_1000 = 0
        gaps_2000 = 0
        gaps_2500 = 0

        stream_start_time = time.perf_counter()
        last_read_time = stream_start_time

        while True:
            now = time.perf_counter()
            elapsed_total = now - stream_start_time
            if elapsed_total >= duration_sec:
                break

            chunk = resp.read(chunk_size)
            now_read = time.perf_counter()

            if not chunk:
                print("Stream closed by server / read returned empty")
                break

            interval_ms = (now_read - last_read_time) * 1000.0
            last_read_time = now_read

            total_bytes += len(chunk)
            read_count += 1
            intervals.append(interval_ms)

            if interval_ms > 250:
                gaps_250 += 1
                print(f"GAP {interval_ms:.1f} ms, received {len(chunk)} bytes (total {total_bytes} bytes)")
            if interval_ms > 500:
                gaps_500 += 1
            if interval_ms > 1000:
                gaps_1000 += 1
            if interval_ms > 2000:
                gaps_2000 += 1
            if interval_ms > 2500:
                gaps_2500 += 1

        resp.close()

        total_duration = time.perf_counter() - stream_start_time
        bytes_per_sec = total_bytes / total_duration if total_duration > 0 else 0
        avg_gap = (sum(intervals) / len(intervals)) if intervals else 0
        max_gap = max(intervals) if intervals else 0

        res = {
            'url': url,
            'final_url': final_url,
            'status': status_code,
            'content_type': content_type,
            'total_bytes': total_bytes,
            'total_duration': total_duration,
            'bytes_per_sec': bytes_per_sec,
            'read_count': read_count,
            'avg_gap': avg_gap,
            'max_gap': max_gap,
            'gaps_250': gaps_250,
            'gaps_500': gaps_500,
            'gaps_1000': gaps_1000,
            'gaps_2000': gaps_2000,
            'gaps_2500': gaps_2500
        }

        print("\n--- TEST SUMMARY ---")
        print(f"Total Bytes:      {total_bytes} bytes")
        print(f"Total Duration:   {total_duration:.2f} s")
        print(f"Speed:            {bytes_per_sec:.1f} bytes/sec ({bytes_per_sec*8/1000:.1f} kbps)")
        print(f"Read Count:       {read_count}")
        print(f"Avg Interval:     {avg_gap:.1f} ms")
        print(f"Max Interval:     {max_gap:.1f} ms")
        print(f"Intervals >250ms: {gaps_250}")
        print(f"Intervals >500ms: {gaps_500}")
        print(f"Intervals >1s:    {gaps_1000}")
        print(f"Intervals >2s:    {gaps_2000}")
        print(f"Intervals >2.5s:  {gaps_2500}")

        return res

    except Exception as e:
        print(f"ERROR testing {url}: {e}")
        return {
            'url': url,
            'error': str(e)
        }

if __name__ == '__main__':
    urls = [
        "https://stream2.datacenter.by/dushevnoe",
        "http://87.244.47.90:8000/rh"
    ]

    results = []
    for u in urls:
        r = test_stream(u, duration_sec=35, chunk_size=2048)
        results.append(r)

    print("\n=========================================================================================================")
    print("COMPARATIVE SUMMARY TABLE")
    print("=========================================================================================================")
    header = f"{'URL':<40} | {'avg gap':<8} | {'max gap':<8} | {'>250ms':<6} | {'>500ms':<6} | {'>1s':<5} | {'>2s':<5} | {'>2.5s':<5} | {'bytes/sec':<10}"
    print(header)
    print("-" * len(header))

    for r in results:
        if 'error' in r:
            print(f"{r['url']:<40} | ERROR: {r['error']}")
        else:
            u_short = r['url'].replace("https://", "").replace("http://", "")
            if len(u_short) > 38:
                u_short = u_short[:35] + "..."
            line = f"{u_short:<40} | {r['avg_gap']:<8.1f} | {r['max_gap']:<8.1f} | {r['gaps_250']:<6} | {r['gaps_500']:<6} | {r['gaps_1000']:<5} | {r['gaps_2000']:<5} | {r['gaps_2500']:<5} | {r['bytes_per_sec']:<10.1f}"
            print(line)
