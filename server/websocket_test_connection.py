import asyncio
import websockets
import time
import socket

async def connect():
    uri = "ws://painters.segerend.nl:80"
    while True:
        try:
            # Attempt to establish a WebSocket connection
            async with websockets.connect(uri) as websocket:
                print("Connected to server")
                
                # Send an initial message
                await websocket.send("Hello, Server!")

                # Continuously receive and print messages from the server
                while True:
                    response = await websocket.recv()
                    print(f"Message from server: {response}")
        
        except (websockets.exceptions.ConnectionClosed, ConnectionRefusedError, socket.gaierror) as e:
            # Handle connection errors and print the exception
            print(f"Connection failed: {e}. Retrying...")
        
        # Wait before trying to reconnect
        time.sleep(5)

# Run the asyncio event loop
asyncio.get_event_loop().run_until_complete(connect())
