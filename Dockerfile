# #############################################################################
# Stage 1: Builder
# This stage compiles the C++ application from source.
# #############################################################################
FROM ubuntu:22.04 AS builder

# Set non-interactive mode for package installations
ENV DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-c"]

# Install all build-time dependencies in a single layer to optimize caching
RUN apt-get update && \
    apt-get install -y \
    curl \
    wget \
    git \
    cmake \
    build-essential \
    python3-pip \
    make \
    g++ \
    libcurl4-openssl-dev \
    aria2 \
    ffmpeg \
    libopencv-dev \
    libboost-all-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone the repository and its submodules, then build the application
WORKDIR /app
RUN git clone https://github.com/nice-repo/duix.ai.core.git . && \
    git submodule update --init --recursive && \
    mkdir -p build && cd build && \
    cmake /app && \
    make -j$(nproc)

# #############################################################################
# Stage 2: Final Image
# This stage creates the final, smaller image with only runtime dependencies.
# #############################################################################
FROM ubuntu:22.04

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV GROQ_API_KEY=${GROQ_API_KEY} \
    LM_API_KEY=${LM_API_KEY}
ENV HF_ENDPOINT=https://hf-mirror.com
# Combine all PATH modifications into one for clarity and to ensure venv is prioritized
ENV PATH="/app/audio/.venv/bin:/root/.local/bin:${PATH}"
SHELL ["/bin/bash", "-c"]

# Install all runtime OS dependencies in a single layer
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    ffmpeg \
    portaudio19-dev \
    supervisor \
    python3 \
    libportaudio2 \
    libasound2-dev \
    libopencv-dev \
    libcurl4-openssl-dev \
    curl \
    wget \
    unzip \
    ca-certificates \
    aria2 \
    && rm -rf /var/lib/apt/lists/*

# Create necessary directories
RUN mkdir -p /var/log/supervisor /app/roles /app/audio /app/video

# Set working directory
WORKDIR /app

# ---- FIX: Copy the application first to avoid overwriting models later ----
COPY --from=builder /app /app

# Download large models and resources. This layer will be cached after the first build.
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

# Install Python dependencies. This layer is only rebuilt if requirements.txt changes.
WORKDIR /app/audio
COPY requirements.txt /app/audio/requirements.txt
# ---- FIX: Added `uv init` to create the project file before creating the venv ----
RUN curl -LsS https://astral.sh/uv/install.sh | sh && \
    uv init && \
    uv venv && \
    uv pip install -r /app/audio/requirements.txt && \
    mkdir -p models && \
    bash hfd.sh FunAudioLLM/SenseVoiceSmall && \
    # Add a cache directory for models downloaded at runtime
    mkdir -p /root/.cache && chmod -R 777 /root/.cache

# Final configuration and container startup
COPY supervisord.conf /etc/supervisor/supervisord.conf

# Clean up supervisor socket if it exists
RUN rm -f /var/run/supervisor.sock

# Expose necessary ports
EXPOSE 8080 6001-6003

# Set the default command to run Supervisor
CMD ["/usr/bin/supervisord", "-c", "/etc/supervisor/supervisord.conf"]
