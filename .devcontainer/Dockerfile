FROM docker.io/zmkfirmware/zmk-dev-arm:4.1

RUN apt update && apt install -y --no-install-recommends \
    gh \
    jq \
    && apt clean && rm -rf /var/lib/apt/lists/*

# Install Claude Code (to local user)
RUN curl -fsSL https://claude.ai/install.sh | bash
RUN echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc

# Install Codex CLI & GitHub Copilot CLI
RUN npm install -g @openai/codex @github/copilot

# Install pre-commit
RUN pip3 install pre-commit --break-system-packages

ARG GIT_DELTA_VERSION=0.18.2
RUN ARCH=$(dpkg --print-architecture) && \
    wget "https://github.com/dandavison/delta/releases/download/${GIT_DELTA_VERSION}/git-delta_${GIT_DELTA_VERSION}_${ARCH}.deb" && \
    dpkg -i "git-delta_${GIT_DELTA_VERSION}_${ARCH}.deb" && \
    rm "git-delta_${GIT_DELTA_VERSION}_${ARCH}.deb"

# I'm living in Japan!
ENV TZ="Asia/Tokyo"
ENV DEVCONTAINER=true

RUN git config --global --add safe.directory /workspace
WORKDIR /workspace
