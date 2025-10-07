# #############################################################################
# Stage 1: Builder
# Compiles the C++ application. This stage runs only when its source changes.
# #############################################################################
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

# Install build-time dependencies
RUN apt-get update && \
    apt-get install -y \
    curl wget git cmake build-essential python3-pip make g++ \
    libcurl4-openssl-dev aria2 ffmpeg libopencv-dev libboost-all-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone and build the application
WORKDIR /app
RUN git clone https://github.com/nice-repo/duix.ai.core.git . && \
    git submodule update --init --recursive && \
    mkdir -p build && cd build && \
    cmake /app && \
    make -j$(nproc)

# #############################################################################
# Stage 2: Final Image
# Creates the final, self-contained, and cache-optimized image.
# #############################################################################
FROM ubuntu:22.04

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV GROQ_API_KEY=${GROQ_API_KEY} \
    LM_API_KEY=${LM_API_KEY}
ENV HF_ENDPOINT=https://hf-mirror.com
ENV PATH="/app/audio/.venv/bin:/root/.local/bin:${PATH}"
SHELL ["/bin/bash", "-c"]

# ---- Layer 1: OS Dependencies (Changes rarely) ----
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    ffmpeg portaudio19-dev supervisor python3 libportaudio2 libasound2-dev \
    libopencv-dev libcurl4-openssl-dev curl wget unzip ca-certificates aria2 \
    && rm -rf /var/lib/apt/lists/*

# Create directories
RUN mkdir -p /var/log/supervisor /app/roles /app/audio /app/video

WORKDIR /app

# ---- Layer 2: Large Model Downloads (Changes very rarely) ----
# This layer is now cached. It will NOT re-run unless you change these URLs.
RUN if [ ! -d "gj_dh_res" ]; then \
    wget -q --no-check-certificate https://cdn.guiji.ai/duix/location/gj_dh_res.zip && \
    unzip -q gj_dh_res.zip && rm gj_dh_res.zip; \
    fi && \
    if [ ! -d "roles/SiYao" ]; then \
    wget -q --no-check-certificate https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194450521/siyao_20240418.zip && \
    unzip -q siyao_20240418.zip && mv siyao_20240418 "roles/SiYao" && rm siyao_20240418.zip; \
    fi && \
    if [ ! -d "roles/DearSister" ]; then \
    wget -q --no-check-certificate https://digital-public.obs.cn-east-3.myhuaweicloud.com/duix/digital/model/1719194007931/bendi1_0329.zip && \
    unzip -q bendi1_0329.zip && mv bendi1_0329 "roles/DearSister" && rm bendi1_0329.zip; \
    fi

# ---- Layer 3: Python Dependencies (Changes only when requirements.txt does) ----
WORKDIR /app/audio
COPY requirements.txt /app/audio/requirements.txt
RUN curl -LsS https://astral.sh/uv/install.sh | sh && \
    uv init && \
    uv venv && \
    uv pip install -r /app/audio/requirements.txt && \
    mkdir -p models && \
    bash hfd.sh FunAudioLLM/SenseVoiceSmall && \
    mkdir -p /root/.cache && chmod -R 777 /root/.cache

# ---- Layer 4: Application Code (Changes frequently) ----
# Because this is the LAST major step, changes to your code will only
# cause this layer and the ones below it to be rebuilt. The layers above
# with the heavy downloads will be reused from the cache.
WORKDIR /app
COPY --from=builder /app /app

# ---- Final Layers: Configuration (Changes rarely) ----
COPY supervisord.conf /etc/supervisor/supervisord.conf
RUN rm -f /var/run/supervisor.sock
EXPOSE 8080 6001-6003
CMD ["/usr/bin/supervisord", "-c", "/etc/supervisor/supervisord.conf"]
