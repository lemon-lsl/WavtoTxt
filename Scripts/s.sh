#!/bin/bash

# ctrl+c -> exit shell
trap "echo -e '\nStopping recording...'; pkill -9 arecord; exit 0" SIGINT SIGTERM

# Wav dir
SAVE_DIR="$HOME/WavtoTxt/Wav"

# exit?
if [ ! -d "${SAVE_DIR}" ]; then
    mkdir -p "${SAVE_DIR}"
    else
    echo "${SAVE_DIR} has exited."
fi

echo "Starting loop recording..."
echo "Save directory: ${SAVE_DIR}."
echo "Press Ctrl+C to stop recording."

while true

do
    # Get current date and time (Year Month Day Hour Minute Second)
    TIMESTAMP=$(date +"%Y_%m_%d_%H_%M_%S")

    # Generate WAV filename
    WAV_FILE="${SAVE_DIR}/${TIMESTAMP}.wav"

    echo "Recording: ${WAV_FILE}"

    # Record 3 seconds of audio
    arecord -D hw:4,0 \
            -c 6 \
            -r 48000 \
            -f S16_LE \
            -d 3 \
            "${WAV_FILE}"

    echo "Recording finished: ${WAV_FILE}"

done
