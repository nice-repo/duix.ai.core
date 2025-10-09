import asyncio
import websockets
import json
import numpy as np
from silvad import SileroVAD
from funasr import AutoModel

# --- Configuration ---
# Using the lightweight SenseVoiceSmall model from Hugging Face for good performance on CPU.
ASR_MODEL = "FunAudioLLM/SenseVoiceSmall"
HOST = "0.0.0.0"
PORT = 6002
VAD_FRAME_SIZE = 512   # Process audio in chunks of 512 samples for VAD

# --- Model Initialization ---
try:
    print("--- Initializing VAD and ASR models ---")
    VAD = SileroVAD()
    # Initialize the model to run on CPU and download from the Hugging Face hub
    ASR = AutoModel(model=ASR_MODEL, hub="hf", device="cpu")
    print("--- Models initialized successfully ---")
except Exception as e:
    print(f"Error initializing models: {e}")
    exit(1)


async def handle_client(websocket):
    """
    This function is called for each new client that connects to the WebSocket server.
    It handles audio buffering, voice activity detection, and transcription.
    """
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

                # Audio data comes from the browser as a list of numbers
                audio_chunk = np.array(data['audioData'], dtype=np.int16)
                audio_buffer = np.concatenate((audio_buffer, audio_chunk))

                # Process the audio in fixed-size chunks for VAD
                while len(audio_buffer) >= VAD_FRAME_SIZE:
                    vad_chunk = audio_buffer[:VAD_FRAME_SIZE]
                    audio_buffer = audio_buffer[VAD_FRAME_SIZE:]

                    # Use the VAD to detect if speech is present in the chunk
                    vad_result = VAD.is_vad(vad_chunk)

                    if is_speaking:
                        # If we are in a speech segment, keep adding audio to the speech_buffer
                        speech_buffer = np.concatenate((speech_buffer, vad_chunk))

                    if vad_result and 'start' in vad_result and not is_speaking:
                        print("Speech start detected.")
                        is_speaking = True
                        # When speech starts, add the triggering chunk to the speech_buffer
                        speech_buffer = np.concatenate((speech_buffer, vad_chunk))
                    
                    if vad_result and 'end' in vad_result and is_speaking:
                        print("Speech end detected. Preparing for transcription...")
                        is_speaking = False
                        
                        # --- FIX: NON-BLOCKING TRANSCRIPTION ---
                        
                        # 1. The model expects audio data as 32-bit floats, not 16-bit integers.
                        #    Convert the buffer and normalize it to the range [-1.0, 1.0].
                        speech_float32 = speech_buffer.astype(np.float32) / 32768.0
                        
                        # 2. ASR.generate() is a blocking, CPU-intensive function.
                        #    We run it in a separate thread using run_in_executor to avoid freezing the server.
                        loop = asyncio.get_running_loop()
                        results = await loop.run_in_executor(
                            None,              # Use the default thread pool executor
                            ASR.generate,      # The function to run in the thread
                            speech_float32     # The argument for the function
                        )
                        # --- END FIX ---
                        
                        transcribed_text = ""
                        if results and len(results) > 0 and "text" in results[0]:
                            transcribed_text = results[0]["text"]
                        
                        # Clear the buffer for the next utterance
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
        print("\nServer is shutting down.")
