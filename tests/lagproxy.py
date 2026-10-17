# TCP relay adding one-way delay and a bandwidth cap per direction.
import asyncio, sys, time
LISTEN, TARGET = int(sys.argv[1]), int(sys.argv[2])
DELAY = float(sys.argv[3]) / 1000          # one-way delay, seconds
UP = float(sys.argv[4]) * 1e6 / 8          # client->server bytes/s
DOWN = float(sys.argv[5]) * 1e6 / 8        # server->client bytes/s
# optional: cut the first connection after this many client->server bytes (tests reconnects)
DROP_AFTER = int(sys.argv[6]) if len(sys.argv) > 6 else 0
dropped = False

async def pump(reader, writer, rate, limit=0, peer=None):
    q = asyncio.Queue()
    async def sender():
        while True:
            due, data = await q.get()
            if data is None:
                writer.close(); return
            d = due - time.monotonic()
            if d > 0: await asyncio.sleep(d)
            writer.write(data); await writer.drain()
    t = asyncio.create_task(sender())
    global dropped
    wire_free = 0.0
    total = 0
    while True:
        data = await reader.read(16384)
        total += len(data)
        if limit and not dropped and total > limit:
            dropped = True
            writer.transport.abort(); peer.transport.abort()
            return
        now = time.monotonic()
        if not data:
            await q.put((now + DELAY, None)); break
        wire_free = max(wire_free, now) + len(data) / rate
        # backpressure: don't read further ahead than the link can carry
        if wire_free - now > 0.05: await asyncio.sleep(wire_free - now - 0.05)
        await q.put((wire_free + DELAY, data))
    await t

async def handle(cr, cw):
    sr, sw = await asyncio.open_connection('127.0.0.1', TARGET)
    await asyncio.gather(pump(cr, sw, UP, DROP_AFTER, cw), pump(sr, cw, DOWN), return_exceptions=True)

async def main():
    srv = await asyncio.start_server(handle, '127.0.0.1', LISTEN)
    async with srv: await srv.serve_forever()
asyncio.run(main())
