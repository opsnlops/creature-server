#!/usr/bin/env bash
#
# Download whisper model and CMU dictionary for local development.
#
# Files are placed in data/ at the project root. Point the server at them with:
#   --whisper-model-path data/ggml-base.en.bin --cmu-dict-path data/cmudict.dict
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${SCRIPT_DIR}/data"

WHISPER_URL="https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin"
WHISPER_FILE="${DATA_DIR}/ggml-base.en.bin"

CMU_URL="https://raw.githubusercontent.com/cmusphinx/cmudict/master/cmudict.dict"
CMU_FILE="${DATA_DIR}/cmudict.dict"

mkdir -p "${DATA_DIR}"

download() {
    local url="$1" dest="$2" label="$3"

    if [[ -f "${dest}" ]]; then
        echo "${label} already exists: ${dest}"
        return
    fi

    echo "Downloading ${label}..."
    curl -L --progress-bar -o "${dest}.tmp" "${url}"
    mv "${dest}.tmp" "${dest}"
    echo "${label} saved to ${dest}"
}

download "${WHISPER_URL}" "${WHISPER_FILE}" "whisper base.en model (~150 MB)"
download "${CMU_URL}"     "${CMU_FILE}"     "CMU Pronouncing Dictionary (~4 MB)"

echo ""
echo "Done. Pass these to creature-server:"
echo "  --whisper-model-path ${WHISPER_FILE} \\"
echo "  --cmu-dict-path ${CMU_FILE}"
