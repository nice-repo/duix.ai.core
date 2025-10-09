import asyncio
import websockets
import json
import numpy as np
from collections import deque # --- NEW: Import deque for efficient buffering ---
from silvad import SileroVAD
from funasr import AutoModel
from funasr.utils.postprocess_utils import rich_transcription_postprocess

# --- Configuration ---
##ASR_MODEL = "FunAudioLLM/SenseVoiceSmall"
ASR_MODEL = "/app/audio/SenseVoiceSmall"
HOST = "0.0.0.0"
PORT = 6002
VAD_FRAME_SIZE = 512

# Number of silent chunks required to trigger transcription (~0.5 seconds)
SILENCE_THRESHOLD = 15 
# Number of audio chunks to keep as padding before speech starts (~0.3 seconds)
PADDING_CHUNKS = 10

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
    silence_counter = 0
    # --- NEW: A rolling buffer to store pre-speech audio padding ---
    padding_buffer = deque(maxlen=PADDING_CHUNKS)
    
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

                    # --- MODIFIED: More robust VAD and buffering logic ---
                    if not is_speaking:
                        # While not speaking, continuously update the padding buffer
                        padding_buffer.append(vad_chunk)

                    vad_result = VAD.is_vad(vad_chunk)

                    if vad_result and 'start' in vad_result:
                        if not is_speaking:
                            print("Speech start detected.")
                            is_speaking = True
                            # When speech starts, initialize the speech buffer with the padding
                            speech_buffer = np.concatenate(list(padding_buffer))
                        else:
                            # Speech continues, add the current chunk
                            speech_buffer = np.concatenate((speech_buffer, vad_chunk))
                        silence_counter = 0
                    
                    elif is_speaking:
                        # Append the current chunk to the speech buffer
                        speech_buffer = np.concatenate((speech_buffer, vad_chunk))
                        
                        # Check for silence
                        if not vad_result:
                            silence_counter += 1
                        else:
                            silence_counter = 0

                        if silence_counter >= SILENCE_THRESHOLD:
                            print(f"Sustained silence detected. Transcribing...")
                            
                            speech_float32 = speech_buffer.astype(np.float32) / 32768.0
                            
                            loop = asyncio.get_running_loop()
                            results = await loop.run_in_executor(
                                None,
                                lambda: ASR.generate(input=speech_float32, language="en")
                            )
                            
                            transcribed_text = ""
                            if results and len(results) > 0 and "text" in results[0]:
                                raw_text_with_tags = results[0]["text"]
                                transcribed_text = rich_transcription_postprocess(raw_text_with_tags)
                            
                            # Reset state for the next utterance
                            is_speaking = False
                            speech_buffer = np.array([], dtype=np.int16)
                            padding_buffer.clear()
                            silence_counter = 0
                            
                            if transcribed_text:
                                print(f"Clean Transcription: '{transcribed_text}'")
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
