import socket
import ssl
import h2.connection
import h2.config
import h2.events

def test_h2_basic():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.set_alpn_protocols(["h2"])

    sock = socket.create_connection(("127.0.0.1", 8080))
    ssock = ctx.wrap_socket(sock, server_hostname="localhost")

    alpn = ssock.selected_alpn_protocol()
    print(f"ALPN negotiated: {alpn}")

    if alpn != "h2":
        print("FAIL: ALPN did not negotiate h2")
        ssock.close()
        return False

    config = h2.config.H2Configuration(client_side=True)
    conn = h2.connection.H2Connection(config=config)
    conn.initiate_connection()
    ssock.sendall(conn.data_to_send())

    response_data = {}

    stream_id = conn.get_next_available_stream_id()
    conn.send_headers(stream_id, [
        (":method", "GET"),
        (":path", "/api/test"),
        (":scheme", "https"),
        (":authority", "localhost:8080"),
    ])
    conn.send_data(stream_id, b"", end_stream=True)
    ssock.sendall(conn.data_to_send())

    import time
    deadline = time.time() + 5.0
    done = False

    while time.time() < deadline and not done:
        try:
            ssock.settimeout(1.0)
            data = ssock.recv(65535)
            if not data:
                print("Connection closed by server")
                break

            events = conn.receive_data(data)
            for event in events:
                if isinstance(event, h2.events.ResponseReceived):
                    headers = dict(event.headers)
                    status = headers.get(b":status", b"?").decode()
                    print(f"Stream {event.stream_id}: status={status}")
                    for k, v in event.headers:
                        if not k.startswith(b":"):
                            print(f"  {k.decode()}: {v.decode()}")
                    response_data["status"] = status
                    response_data["headers"] = headers

                elif isinstance(event, h2.events.DataReceived):
                    body = event.data.decode("utf-8", errors="replace")
                    print(f"Stream {event.stream_id}: data ({len(event.data)} bytes): {body[:200]}")
                    response_data.setdefault("body", "")
                    response_data["body"] += body
                    conn.acknowledge_received_data(event.flow_controlled_length, event.stream_id)

                elif isinstance(event, h2.events.StreamEnded):
                    print(f"Stream {event.stream_id}: ended")
                    done = True

                elif isinstance(event, h2.events.StreamReset):
                    print(f"Stream {event.stream_id}: reset with error {event.error_code}")
                    done = True

                elif isinstance(event, h2.events.RemoteSettingsChanged):
                    print(f"Remote settings changed: {event.changed_settings}")

                elif isinstance(event, h2.events.SettingsAcknowledged):
                    pass

                elif isinstance(event, h2.events.WindowUpdated):
                    pass

                else:
                    print(f"Event: {type(event).__name__}: {event}")

            outgoing = conn.data_to_send()
            if outgoing:
                ssock.sendall(outgoing)
        except socket.timeout:
            continue
        except Exception as e:
            print(f"Error: {e}")
            break

    ssock.close()

    if "status" in response_data:
        print(f"\nSUCCESS: Got HTTP/2 response with status {response_data['status']}")
        return True
    else:
        print("\nFAIL: No response received")
        return False

if __name__ == "__main__":
    test_h2_basic()
