from dataclasses import dataclass
import json
import asyncio
from websockets.asyncio.client import connect
import serial.tools.list_ports
import serial

# SERVER = "bar.emf.camp"
SERVER = "localhost"
HTTP_PORT = 8000
WEBSOCKET_PORT = 8001
HTTP_POLL_INTERVAL = 10  # seconds, if websockets are down
HTTP_PATH = "/api/stocklines.json?output=full&location=Bar"
WEBSOCKET_PATH = "/websocket"
WEBSOCKET_RETRY_INTERVAL = 5  # seconds

SERIAL_VID = 0x303A
SERIAL_BAUD = 115200
SERIAL_TIMEOUT = 1
SERIAL_SCAN_INTERVAL = 2  # seconds

serial_tasks_by_port: dict[str, asyncio.Task] = {}
serial_state_lock = asyncio.Lock()

@dataclass
class Pump:
    name: str = ""
    manufacturer: str = ""
    note: str = ""
    abv: float = -1
    total_pints: int = 100
    remaining_pints: int = 0

pumps = {
    1: Pump(name=f"{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}", manufacturer="No ws conn at:"),  # pump 1
    2: Pump(name=f"{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}", manufacturer="No ws conn at:"),  # pump 2
    3: Pump(name=f"{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}", manufacturer="No ws conn at:"),  # pump 3
    4: Pump(name=f"{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}", manufacturer="No ws conn at:")   # pump 4
}

# Which pump each serial port is currently displaying (set after first poll line)
port_to_pump: dict[str, int] = {}

# Open serial handles by port name
serial_by_port: dict[str, serial.Serial] = {}

# Optional: per-port write lock to prevent interleaved writes
port_write_locks: dict[str, asyncio.Lock] = {}

def set_pump_to_empty(pump_id: int):
    pumps[pump_id] = Pump(name="#ff0000 Out of service!#", manufacturer="")

def set_pump_to_ws_error(pump_id: int):
    pumps[pump_id] = Pump(name=f"{SERVER}:{WEBSOCKET_PORT}{WEBSOCKET_PATH}", manufacturer="#ff0000 WS Connection Error!#")

def update_pump_from_message(message: str) -> int | None:
    try:
        data = json.loads(message)
        pump_id = int(data.get("id", 0) - 99)  # 100-103 -> 1-4
        if not (1 <= pump_id <= 4):
            return None

        stockitem = data.get("stockitem") or {}
        stocktype = stockitem.get("stocktype") or {}
        if not stocktype:
            set_pump_to_empty(pump_id)
            return None

        pumps[pump_id] = Pump(
            name=stocktype.get("name", ""),
            manufacturer=stocktype.get("manufacturer", ""),
            note=data.get("note", ""),
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
    note = (p.note or "").replace("\n", " ").replace(",", " ")
    return f"{pump_id}:{name},{mfr},{note},{p.abv},{p.total_pints},{p.remaining_pints}\n"
    
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
        except (serial.SerialException, OSError, PermissionError) as e:
            print(f"Push failed on {port}: {e}")
            await close_serial_port(port, reason="push write failed")
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
                    if isinstance(message, bytes):
                        message = message.decode("utf-8")
                    updated = update_pump_from_message(message)
                    print(f"Received update for pump {updated}: {pumps.get(updated)}")
                    if updated is not None:
                        await push_pump_update(updated)
        except Exception as e:
            print(f"WebSocket connection error: {e}")
            print(f"Retrying in {WEBSOCKET_RETRY_INTERVAL} seconds...")
            for pump_id in range(1, 5):
                set_pump_to_ws_error(pump_id)
                await push_pump_update(pump_id)
            await asyncio.sleep(WEBSOCKET_RETRY_INTERVAL)

def list_target_ports() -> set[str]:
    ports = serial.tools.list_ports.comports()
    return {p.device for p in ports if p.vid == SERIAL_VID}

async def open_serial_port(port: str) -> None:
    async with serial_state_lock:
        if port in serial_by_port:
            return

    try:
        ser = await asyncio.to_thread(serial.Serial, port, SERIAL_BAUD, timeout=SERIAL_TIMEOUT)
    except Exception as e:
        print(f"Error opening serial port {port}: {e}")
        return

    async with serial_state_lock:
        serial_by_port[port] = ser
        port_write_locks[port] = asyncio.Lock()
        serial_tasks_by_port[port] = asyncio.create_task(handle_serial_port(port))
    print(f"Opened serial port: {port}")

async def close_serial_port(port: str, reason: str = "", *, cancel_task: bool = True) -> None:
    async with serial_state_lock:
        task = serial_tasks_by_port.pop(port, None)
        ser = serial_by_port.pop(port, None)
        port_write_locks.pop(port, None)
        port_to_pump.pop(port, None)

    if cancel_task and task:
        task.cancel()

    if ser:
        try:
            await asyncio.to_thread(ser.close)
        except Exception as e:
            print(f"Error closing serial port {port}: {e}")

    if reason:
        print(f"Closed serial port {port}: {reason}")

async def handle_serial_port(port: str):
    try:
        while True:
            async with serial_state_lock:
                ser = serial_by_port.get(port)
            if ser is None:
                return  # closed elsewhere

            raw = await asyncio.to_thread(ser.readline)
            if not raw:
                continue

            line = raw.decode(errors="ignore")
            pump_id = parse_pump_request_line(line)
            if pump_id is None:
                continue

            if port in port_to_pump and port_to_pump[port] != pump_id:
                print(f"Changing pump on {port} from {port_to_pump[port]} to {pump_id}")
            elif port not in port_to_pump:
                print(f"Adding {port} to pump {pump_id}")
            port_to_pump[port] = pump_id
            reply = pump_to_reply_line(pump_id)
            async with port_write_locks[port]:
                await asyncio.to_thread(ser.write, reply.encode("utf-8"))

    except asyncio.CancelledError:
        raise
    except (serial.SerialException, OSError, PermissionError) as e:
        print(f"Serial error on {port}: {e}")
        await close_serial_port(port, reason="read/write error", cancel_task=False)
    
async def serial_port_manager():
    while True:
        desired = list_target_ports()
        async with serial_state_lock:
            current = set(serial_by_port.keys())

        for port in desired - current:
            await open_serial_port(port)

        for port in current - desired:
            await close_serial_port(port, reason="disconnected")

        await asyncio.sleep(SERIAL_SCAN_INTERVAL)

async def main():
    serial_manager_task = asyncio.create_task(serial_port_manager())
    ws_task = asyncio.create_task(websocket_handler())
    await asyncio.gather(ws_task, serial_manager_task)

if __name__ == "__main__":
    asyncio.run(main())