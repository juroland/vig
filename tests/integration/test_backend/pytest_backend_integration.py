import multiprocessing
import socket
import time
from datetime import datetime

import uvicorn
from fastapi import FastAPI, Header, HTTPException, Request, Response
from pydantic import BaseModel, Field
from pytest_embedded import Dut

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

class MotionEventRequest(BaseModel):
    """Payload for a device-reported motion capture event."""

    hardware_id: str = Field(..., min_length=1, max_length=128)
    timestamp: datetime | None = None
    capture: str = Field(..., description="Base64 encoded JPEG capture")


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
    if authorization != "Bearer test_token_123":
        raise HTTPException(status_code=401)
    
    if req.hardware_id != "SIM_CAM_001":
        raise HTTPException(status_code=400)

    return {
        "ack": True,
        "update_available": False,
        "stream_token": "mock_jwt_stream_token",
        "whip_url": f"http://{get_local_ip()}:8081/whip"
    }

@app.post("/api/devices/offline")
async def offline(req: Offline, authorization: str = Header(None)):
    if authorization != "Bearer test_token_123":
        raise HTTPException(status_code=401)
    return {"status": "ok"}

@app.post("/api/devices/motion")
@app.post("/motion")
async def motion(req: MotionEventRequest, authorization: str = Header(None)):
    if authorization != "Bearer test_token_123":
        raise HTTPException(status_code=401)
    return {"status": "ok"}


@app.post("/whip")
async def whip(req: Request, authorization: str = Header(None)):
    if authorization != "Bearer mock_jwt_stream_token":
        raise HTTPException(status_code=401)
    
    sdp_offer = await req.body()
    if b"m=video" not in sdp_offer:
        raise HTTPException(status_code=400)
    
    ans = f"v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Mock\r\nc=IN IP4 {get_local_ip()}\r\nt=0 0\r\nm=video 5004 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
    return Response(content=ans, media_type="application/sdp")

def run_server():
    uvicorn.run(app, host="0.0.0.0", port=8081, log_level="info")

import pytest

@pytest.fixture(autouse=True)
def mock_server():
    server_process = multiprocessing.Process(target=run_server, daemon=True)
    server_process.start()
    time.sleep(2)  # Wait for server to be ready
    yield
    server_process.terminate()
    server_process.join()

def test_backend_integration(dut: Dut):
    # Let pytest-embedded parse the Unity output
    dut.expect_unity_test_output(timeout=30)
