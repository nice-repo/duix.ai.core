import asyncio
import websockets
import json
import wave
import os
import numpy as np
from silvad import SileroVAD
from funasr import FunASR

# --- Configuration ---
# This is a larger, more accurate English model from the FunASR Model Zoo.
# It will be downloaded automatically the first time you run the server.
ASR_MODEL = "damo/speech_paraformer-large_asr_nat-en-16k-common-vocab8404-pytorch"

# The server will listen on this host and port.
HOST = "0.0.0.0"
PORT = 6002

# The audio format required by the ASR model.
TARGET_SAMPLE_RATE = 16000
TARGET_CHANNELS = 1
TARGET_SAMPLE_WIDTH = 2  # 2 bytes for 16-bit audio

# Voice Activity Detection (VAD) settings
VAD_PADDING_MS = 512  # Add 512ms of padding before and after speech
VAD_FRAME_SIZE = 512   # Process audio in chunks of 512 samples

# Create instances of the VAD and ASR models
try:
    print("--- Initializing VAD and ASR models ---")
    VAD = SileroVAD()
    ASR = FunASR(model=ASR_MODEL)
    print("--- Models initialized successfully ---")
except Exception as e:
    print(f"Error initializing models: {e}")
    exit(1)


async def handle_client(websocket, path):
    """
    This function is called for each new client that connects to the WebSocket server.
    It manages the audio buffer and state for that specific client.
    """
    print(f"Client connected from {websocket.remote_address}")

    # State variables for this client's connection
    audio_buffer = np.array([], dtype=np.int16)
    is_speaking = False
    
    try:
        async for message in websocket:
            try:
                data = json.loads(message)

                if 'audioData' not in data or 'metadata' not in data:
                    print("Warning: Received message without 'audioData' or 'metadata'")
                    continue

                # The audio data arrives as a list of numbers from the frontend
                audio_chunk = np.array(data['audioData'], dtype=np.int16)
                
                # The SileroVAD model requires chunks of a specific size (e.g., 512 samples for 16kHz)
                # We buffer the incoming audio to create chunks of the correct size.
                audio_buffer = np.concatenate((audio_buffer, audio_chunk))

                while len(audio_buffer) >= VAD_FRAME_SIZE:
                    # Get the next chunk for VAD processing
                    vad_chunk = audio_buffer[:VAD_FRAME_SIZE]
                    audio_buffer = audio_buffer[VAD_FRAME_SIZE:]

                    vad_result = VAD.is_vad(vad_chunk)

                    if vad_result and 'start' in vad_result and not is_speaking:
                        print("Speech start detected.")
                        is_speaking = True
                    
                    if vad_result and 'end' in vad_result and is_speaking:
                        print("Speech end detected. Transcribing...")
                        is_speaking = False
                        
                        # When speech ends, transcribe the entire buffer we've collected
                        transcribed_text = ASR.recognizer(audio_buffer)
                        
                        # Clear the buffer after transcription
                        audio_buffer = np.array([], dtype=np.int16)
                        
                        if transcribed_text:
                            print(f"Transcription result: '{transcribed_text}'")
                            response = json.dumps({
                                'event': 'query',
                                'value': transcribed_text
                            }, ensure_ascii=False)
                            await websocket.send(response)
                        else:
                            print("Transcription was empty.")

            except json.JSONDecodeError:
                print("Received a non-JSON message, ignoring.")
            except KeyError as e:
                print(f"Received JSON is missing a key: {e}")
            except Exception as e:
                print(f"An error occurred while processing audio: {e}")

    except websockets.exceptions.ConnectionClosed as e:
        print(f"Client disconnected: {e.code} {e.reason}")
    finally:
        print(f"Connection with {websocket.remote_address} closed.")


async def main():
    """Starts the WebSocket server."""
    server = await websockets.serve(
        handle_client,
        HOST,
        PORT,
        ping_interval=None,
        ping_timeout=None
    )
    print(f"--- WebSocket ASR Server started on ws://{HOST}:{PORT} ---")
    await server.wait_closed()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("Server is shutting down.")
