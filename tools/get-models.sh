#!/bin/sh
# Downloads Quill's language models (SmolLM2 Instruct and Qwen2.5 Coder
# Instruct, Q4_0 GGUF from bartowski on Hugging Face) into downloads/models
# and checks them. With
# --push it also copies them over SSH to the running Kindle, where Quill
# looks for them.
set -e
cd "$(dirname "$0")/.."
M=downloads/models
mkdir -p $M
HF=https://huggingface.co/bartowski
get() { # repo file sha256 target
    [ -f "$M/$2" ] || curl -fL --retry 3 -o "$M/$2" "$HF/$1/resolve/main/$2"
    echo "$3  $M/$2" | shasum -a 256 -c
    if [ "$PUSH" = 1 ]; then tools/push.sh "$M/$2:/mnt/us/linux/models/$4:644"; fi
}
PUSH=0
if [ "$1" = --push ]; then PUSH=1; ./ssh.sh 'mkdir -p /mnt/us/linux/models'; fi
get SmolLM2-135M-Instruct-GGUF SmolLM2-135M-Instruct-Q4_0.gguf bcc3af2849ad6095af57e9b5cd43775256efdc66e306acb529172f92d0c04b03 smollm2-135m-q4_0.gguf
get SmolLM2-360M-Instruct-GGUF SmolLM2-360M-Instruct-Q4_0.gguf c3608933eb6e5763b87f769bda40c204dc158333668c7af214644fe39da58627 smollm2-360m-q4_0.gguf
get Qwen2.5-Coder-0.5B-Instruct-GGUF Qwen2.5-Coder-0.5B-Instruct-Q4_0.gguf bf52e650e19a33d965431b9b13d1ec2422c2a2be12274eaef1f15f3d5a330c6c qwen2.5-coder-0.5b-q4_0.gguf
