import asyncio
import websockets
import json
import numpy as np
from silvad import SileroVAD
from funasr import AutoModel

# - -- Configuration ---
ASR_MODEL = "FunAudioLLM/SenseVoiceSmall"
HOST = "0.0.0.0"
PORT = 6002
VAD_FRAME_SIZE = 512

# --- Model Initialization ---
try:
    print("--- Initializing VAD and ASR models ---")
    VAD = SileroVAD()
    ASR = AutoModel(model=ASR_MODEL, hub="hf", device="cpu")
    print("--- Models initialized successfully ---")
except Exception as e:
    print(f"Error initializing models: {e}")
    exit(1)


async def handle_client(websocket):
    print(f"Client connected from {websocket.remote_address}")

    audio_buffer = np.array([], dtype=np.int16)
    speech_buffer = np.array([], dtype=np.int16)
    is_speaking = False
    
    try:
        async for message in websocket:
            try:
                data = json.loads(message)
                if 'audioData' not in data or 'metadata' not in data:
                    continue

                audio_chunk = np.array(data['audioData'], dtype=np.int16)
                audio_buffer = np.concatenate((audio_buffer, audio_chunk))

                while len(audio_buffer) >= VAD_FRAME_SIZE:
                    vad_chunk = audio_buffer[:VAD_FRAME_SIZE]
                    audio_buffer = audio_buffer[VAD_FRAME_SIZE:]

                    vad_result = VAD.is_vad(vad_chunk)

                    if is_speaking:
                        speech_buffer = np.concatenate((speech_buffer, vad_chunk))

                    if vad_result and 'start' in vad_result and not is_speaking:
                        print("Speech start detected.")
                        is_speaking = True
                        speech_buffer = np.concatenate((speech_buffer, vad_chunk))
                    
                    if vad_result and 'end' in vad_result and is_speaking:
                        print("Speech end detected. Transcribing...")
                        is_speaking = False
                        
                        speech_float32 = speech_buffer.astype(np.float32) / 32768.0
                        
                        loop = asyncio.get_running_loop()

                        # --- FIX: Wrap the ASR call in a lambda to add the language="en" parameter ---
                        results = await loop.run_in_executor(
                            None,
                            lambda: ASR.generate(input=speech_float32, language="en")
                        )
                        # --- END FIX ---
                        
                        transcribed_text = ""
                        if results and len(results) > 0 and "text" in results[0]:
                            transcribed_text = results[0]["text"]
                        
                        speech_buffer = np.array([], dtype=np.int16)
                        
                        if transcribed_text:
                            print(f"Transcription result: '{transcribed_text}'")
                            response = json.dumps({
                                'event': 'query',
                                'value': transcribed_text
                            }, ensure_ascii=False)
                            await websocket.send(response)
                        else:
                            print("Transcription was empty.")

            except Exception as e:
                print(f"An error occurred while processing audio: {e}")

    except websockets.exceptions.ConnectionClosed as e:
        print(f"Client disconnected: {e.code} {e.reason}")
    finally:
        print(f"Connection with {websocket.remote_address} closed.")


async def main():
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
        print("\nServer is shutting down.")
