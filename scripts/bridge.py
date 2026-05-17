from dataclasses import dataclass
import json
import asyncio
from websockets.asyncio.client import connect
import serial.tools.list_ports

# SERVER = "bar.emf.camp"
SERVER = "localhost"
HTTP_PORT = 8000
WEBSOCKET_PORT = 8001
HTTP_POLL_INTERVAL = 10  # seconds, if websockets are down
HTTP_PATH = "/api/stocklines.json?output=full&location=Bar"
WEBSOCKET_PATH = "/websocket"
WEBSOCKET_RETRY_INTERVAL = 5  # seconds

@dataclass
class Pump:
    name: str = ""
    manufacturer: str = ""
    abv: float = 0
    total_pints: int = 0
    remaining_pints: int = 0

pumps = {
    1: Pump(),  # pump 1
    2: Pump(),  # pump 2
    3: Pump(),  # pump 3
    4: Pump()   # pump 4
}

# Which pump each serial port is currently displaying (set after first poll line)
port_to_pump: dict[str, int] = {}

# Open serial handles by port name
serial_by_port: dict[str, serial.Serial] = {}

# Optional: per-port write lock to prevent interleaved writes
port_write_locks: dict[str, asyncio.Lock] = {}

serial_ports = []

def update_pump_from_message(message: str) -> int | None:
    try:
        data = json.loads(message)
        pump_id = int(data.get("id", 0) - 99)  # 100-103 -> 1-4
        if not (1 <= pump_id <= 4):
            return None

        stockitem = data.get("stockitem") or {}
        stocktype = stockitem.get("stocktype") or {}
        if not stocktype:
            return None

        pumps[pump_id] = Pump(
            name=stocktype.get("name", ""),
            manufacturer=stocktype.get("manufacturer", ""),
            abv=stocktype.get("abv", 0),
            total_pints=stockitem.get("size", 0),
            remaining_pints=stockitem.get("remaining", 0),
        )
        return pump_id
    except Exception as e:
        print(f"Error processing message: {e}")
        return None
    
def parse_pump_request_line(line: str) -> int | None:
    s = line.strip()
    if not s:
        return None
    if not s.isdigit():
        return None
    pump_id = int(s)
    if 1 <= pump_id <= 4:
        return pump_id
    return None
    
def pump_to_reply_line(pump_id: int) -> str:
    p = pumps.get(pump_id, Pump())
    name = (p.name or "").replace("\n", " ").replace(",", " ")
    mfr  = (p.manufacturer or "").replace("\n", " ").replace(",", " ")
    mfr = "Queer Brewing Queer Brewing"
    return f"{pump_id}:{name},{mfr},{p.abv},{p.total_pints},{p.remaining_pints}\n"
    
async def handle_serial_port(port: str):
    ser = serial_by_port[port]
    while True:
        try:
            raw = await asyncio.to_thread(ser.readline)  # blocks in thread
            print(f"Received from {port}: {raw}")
            if not raw:
                continue
            line = raw.decode(errors="ignore")
            pump_id = parse_pump_request_line(line)
            if pump_id is None:
                continue

            port_to_pump[port] = pump_id  # “map has been updated by a display message”

            reply = pump_to_reply_line(pump_id)
            print(f"Replying to {port} with: {reply.strip()}")
            async with port_write_locks[port]:
                await asyncio.to_thread(ser.write, reply.encode("utf-8"))
        except Exception as e:
            print(f"Serial error on {port}: {e}")
            break
    
async def push_pump_update(pump_id: int):
    reply = pump_to_reply_line(pump_id)
    for port, mapped in list(port_to_pump.items()):
        if mapped != pump_id:
            continue
        ser = serial_by_port.get(port)
        if not ser:
            continue
        try:
            async with port_write_locks[port]:
                await asyncio.to_thread(ser.write, reply.encode("utf-8"))
        except Exception as e:
            print(f"Push failed on {port}: {e}")

async def websocket_handler():
    uri = f"ws://{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}"
    while True:
        try:
            async with connect(uri) as websocket:
                print(f"Connected to WebSocket at {uri}")
                for pump_id in range(1, 5):
                    await websocket.send(f"SUBSCRIBE stockline/{pump_id + 99}")
                while True:
                    message = await websocket.recv()
                    updated = update_pump_from_message(message)
                    if updated is not None:
                        await push_pump_update(updated)
        except Exception as e:
            print(f"WebSocket connection error: {e}")
            print(f"Retrying in {WEBSOCKET_RETRY_INTERVAL} seconds...")
            await asyncio.sleep(WEBSOCKET_RETRY_INTERVAL)

def open_serial_ports():
    # Get all serial ports with vendor ID 303A
    global serial_ports
    all_ports = serial.tools.list_ports.comports()
    target_ports = [port.device for port in all_ports if port.vid == 0x303A]
    if not target_ports:
        print("No serial ports with LilyGO/TT-GO vendor ID 303A found.")
    else:
        print(f"Found valid serial ports: {target_ports}")
        for port in target_ports:
            try:
                ser = serial.Serial(port, 115200, timeout=1)
                print(f"Opened serial port: {port}")
                serial_by_port[port] = ser
                port_write_locks[port] = asyncio.Lock()
            except Exception as e:
                print(f"Error opening serial port {port}: {e}")

async def main():
    open_serial_ports()
    serial_tasks = [asyncio.create_task(handle_serial_port(p)) for p in serial_by_port.keys()]
    ws_task = asyncio.create_task(websocket_handler())
    await asyncio.gather(ws_task, *serial_tasks)

if __name__ == "__main__":
    asyncio.run(main())