#!/bin/bash

trap "echo -e '\nStopping recording...'; pkill -9 arecord; exit 0" SIGINT SIGTERM

# Wav dir and TempWav dir
TEMP_DIR="$HOME/WavtoTxt/TempWav"
SAVE_DIR="$HOME/WavtoTxt/Wav"

# exit?
if [ ! -d "${TEMP_DIR}" ]; then
    mkdir -p "${TEMP_DIR}"
else
    echo "${TEMP_DIR} has exited."
fi

if [ ! -d "${SAVE_DIR}" ]; then
    mkdir -p "${SAVE_DIR}"
else
    echo "${SAVE_DIR} has exited."
fi

echo "Starting loop recording..."
echo "Temp save directory: ${TEMP_DIR}."
echo "Save directory: ${SAVE_DIR}."
echo "Press Ctrl+C to stop recording."

while true
do
    TIMESTAMP=$(date +"%Y_%m_%d_%H_%M_%S")

    WAV_FILE="${SAVE_DIR}/${TIMESTAMP}.wav"
    TMP_FILE="${SAVE_DIR}/${TIMESTAMP}.tmp"

    echo "Recording: ${WAV_FILE}"

    arecord -D hw:4,0 \
            -c 6 \
            -r 48000 \
            -f S16_LE \
            -t wav \
            -d 3 \
            "${TMP_FILE}"

    if [ $? -eq 0 ]; then
        mv "${TMP_FILE}" "${WAV_FILE}"
        echo "Recording finished: ${WAV_FILE}"
    else
        echo "[ERROR] Recording failed: ${TMP_FILE}"
        rm -f "${TMP_FILE}"
    fi
done
