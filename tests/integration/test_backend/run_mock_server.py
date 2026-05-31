import socket
import uvicorn
from fastapi import FastAPI, Header, Request, Response
from pydantic import BaseModel

app = FastAPI()

class Telemetry(BaseModel):
    free_heap: int
    uptime: int
    cpu_temp: float

class Heartbeat(BaseModel):
    hardware_id: str
    firmware_version: str
    telemetry: Telemetry
    snapshot: str | None = None

class Offline(BaseModel):
    hardware_id: str

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        IP = s.getsockname()[0]
    except Exception:
        IP = '127.0.0.1'
    finally:
        s.close()
    return IP

@app.post("/api/devices/heartbeat")
async def heartbeat(req: Heartbeat, authorization: str = Header(None)):
    print(f"\n[Heartbeat Received]")
    print(f"  Hardware ID: {req.hardware_id}")
    print(f"  Firmware: {req.firmware_version}")
    print(f"  Auth Header: {authorization}")
    print(f"  Telemetry: {req.telemetry.dict()}")
    if req.snapshot:
        print(f"  Snapshot: present ({len(req.snapshot)} bytes b64)")
    else:
        print(f"  Snapshot: NOT present")

    return {
        "ack": True,
        "update_available": False,
        "stream_token": "stream_token_for_" + req.hardware_id,
        "whip_url": f"http://{get_local_ip()}:8081/whip"
    }

@app.post("/api/devices/offline")
async def offline(req: Offline, authorization: str = Header(None)):
    print(f"\n[Offline Notification]")
    print(f"  Hardware ID: {req.hardware_id}")
    print(f"  Auth: {authorization}")
    return {"status": "ok"}

@app.post("/whip")
async def whip(req: Request, authorization: str = Header(None)):
    print(f"\n[WHIP Handshake]")
    print(f"  Auth: {authorization}")
    
    sdp_offer = await req.body()
    print(f"  SDP Offer:\n{sdp_offer.decode('utf-8', errors='ignore')}")
    
    # Send back answer pointing to port 5004
    ans = f"v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Mock\r\nc=IN IP4 {get_local_ip()}\r\nt=0 0\r\nm=video 5004 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
    return Response(content=ans, media_type="application/sdp")

if __name__ == "__main__":
    local_ip = get_local_ip()
    print(f"Starting standalone mock backend on {local_ip}:8081...")
    uvicorn.run(app, host="0.0.0.0", port=8081, log_level="info")
