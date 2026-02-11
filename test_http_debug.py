"""Debug script: check raw HTTP response bytes from the C++ server."""
import socket

def check_raw_response():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(("127.0.0.1", 8002))
    s.sendall(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")
    resp = s.recv(4096)
    s.close()

    print("=== Raw bytes (hex) ===")
    print(resp.hex())
    print()
    print("=== Raw bytes (repr) ===")
    print(repr(resp))
    print()
    print("=== Decoded ===")
    print(resp.decode("latin-1"))
    print()

    # Check for the specific issue: \r without \n between headers
    headers_end = resp.find(b"\r\n\r\n")
    if headers_end == -1:
        print("ERROR: No \\r\\n\\r\\n found — headers never end!")
        # Check for \n\n
        if resp.find(b"\n\n") != -1:
            print("  Found \\n\\n instead of \\r\\n\\r\\n")
        return

    headers = resp[:headers_end]
    print(f"=== Header section ({len(headers)} bytes) ===")
    # Split by \r\n to show each header line
    lines = headers.split(b"\r\n")
    for i, line in enumerate(lines):
        print(f"  Line {i}: {line!r}")
        # Check for bare \r in the line (would indicate missing \n)
        if b"\r" in line:
            pos = line.find(b"\r")
            print(f"    WARNING: bare \\r at position {pos}!")

    # Check for \r NOT followed by \n
    for i in range(len(headers) - 1):
        if headers[i:i+1] == b"\r" and headers[i+1:i+2] != b"\n":
            print(f"BUG FOUND: \\r at byte {i} NOT followed by \\n (next byte: {headers[i+1:i+2]!r})")

if __name__ == "__main__":
    check_raw_response()
