#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <signal.h>
#include <set>
#include <sys/inotify.h>
#include <queue>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include <cmath>
#include <utility>
#include <functional>
#include <system_error>
#include <cstdint>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <chrono>

#include <whisper.h>
#include <sndfile.h>
#include <samplerate.h>

const std::string ROOT_DIR = "/home/lsl/WavtoTxt/";
// directories
const std::string INPUT_DIR = ROOT_DIR + "Wav";

const std::string OUTPUT_DIR = ROOT_DIR + "Txt";

const std::string RESULT_FILE = ROOT_DIR + 
    "Txt/result.txt";

const std::string RECORD_FILE = ROOT_DIR + 
    "Txt/processed.txt";

const std::string MODEL_FILE = ROOT_DIR + 
    "WhisperModule/ggml-base.bin";

// Whisper thread nums
const int WHISPER_THREADS = 8;

// =====================================================
// Global variables
// =====================================================

std::atomic<bool> running(true); // atomic

namespace fs = std::filesystem; // directory operation

std::set<std::string> scheduledFiles; // Record filenames avoid duplicate enqueueing

std::mutex queueMutex;  // task queue mutex
std::mutex OutputMutex; // output-file mutex

std::condition_variable queueCV; // condition_variable

bool getTimestamp(const fs::path &path,
                  std::chrono::system_clock::time_point &result);

struct WavPathCompare // Wav timestamp comparison
{
    bool operator()(const fs::path &a,
                    const fs::path &b) const
    {
        std::chrono::system_clock::time_point timeA;
        std::chrono::system_clock::time_point timeB;

        bool validA = getTimestamp(a, timeA);
        bool validB = getTimestamp(b, timeB);

        // Prioritize valid timestamps
        if (validA != validB)
        {
            return !validA;
        }

        // If both timestamps are invalid, sort by filename
        if (!validA)
        {
            return a.filename().string() >
                   b.filename().string();
        }

        // Later times are ordered after earlier ones
        return timeA > timeB;
    }
};

std::priority_queue<
    fs::path,
    std::vector<fs::path>,
    WavPathCompare>
    TaskQueue; // Task queue holding pending wav files

/**
 * @brief Signal handler to gracefully stop the main loop
 *
 * Logic:
 *   1. Set the atomic boolean flag 'running' to false;
 *   2. The main loop, which periodically checks this flag, will exit on its next iteration.
 *
 * @param int The signal number (unused in this implementation)
 * @note Uses an atomic variable to ensure thread-safe and signal-safe communication
 *       with the main thread without relying on unsafe operations inside a signal handler.
 */
void sighandler(int)
{

    running.store(false);
}

/**
 * @brief Extract the timestamp from a filename and convert it to a system_clock time_point
 *
 * Logic:
 *   1. Get the filename (without extension) and parse it using std::get_time with the "%Y_%m_%d_%H_%M_%S" format;
 *   2. If parsing fails or there are extra trailing characters, return false;
 *   3. Convert the parsed std::tm structure to a time_t using std::mktime;
 *   4. If the conversion fails (returns -1), return false;
 *   5. Convert the time_t to a std::chrono::system_clock::time_point and assign it to the result parameter;
 *   6. Return true on success.
 *
 * @param path   The file path from which to extract the timestamp (expects stem to match the specified format)
 * @param result Output parameter to store the parsed timestamp
 * @return true if the timestamp is successfully extracted and parsed; false otherwise
 * @note The filename (excluding extension) must strictly follow the "YYYY_MM_DD_HH_MM_SS" format.
 */
bool getTimestamp(const fs::path &path,
                  std::chrono::system_clock::time_point &result)
{
    std::string filename = path.stem().string();

    std::tm tm = {};

    std::istringstream ss(filename);

    ss >> std::get_time(&tm, "%Y_%m_%d_%H_%M_%S");

    if (ss.fail() || !ss.eof())
    {
        return false;
    }

    std::time_t time = std::mktime(&tm);

    if (time == -1)
    {
        return false;
    }

    result = std::chrono::system_clock::from_time_t(time);

    return true;
}

/**
 * @brief Load the set of processed filenames from a persistent record file
 *
 * Logic:
 *   1. Open the record file defined by RECORD_FILE; if it fails to open, return an empty set;
 *   2. Read the file line by line using std::getline;
 *   3. Ignore empty lines; insert all valid lines into a std::set;
 *   4. Return the populated set (which inherently removes duplicates and sorts the entries).
 *
 * @return A std::set<std::string> containing all previously processed filenames
 * @note The returned set is empty if the file does not exist or cannot be opened.
 */
std::set<std::string> LoadProcessed()
{
    std::set<std::string> result;

    std::ifstream file(RECORD_FILE);

    std::string line;

    while (std::getline(file, line))
    {
        if (!line.empty())
            result.insert(line);
    }
    return result;
}

/**
 * @brief Wait until the file is stable (fully written), used to determine whether a wav is ready for processing
 *
 * Logic:
 *   1. Poll at most 60 times (once per second); exit early if running becomes false;
 *   2. Check file existence each iteration; return false if it does not exist or an error occurs;
 *   3. Read the current file size; on failure, sleep 1 second and retry;
 *   4. If the size is greater than 44 bytes (skipping the wav header) and equals the previous size,
 *      increment the stable counter; otherwise reset it to zero;
 *   5. Two consecutive unchanged sizes indicate the file is fully written, return true;
 *   6. Return false on timeout or early exit.
 *
 * @param path Path of the file to check
 * @return true if the file is stable; false if the file does not exist, cannot be read, or the wait times out
 * @note Uses "two consecutive identical file sizes" to avoid processing a file that is still being written.
 */
bool waitForStableFile(const fs::path &path)
{
    uintmax_t lastSize = 0;
    int stableCount = 0;

    for (int i = 0; i < 60 && running.load(); ++i)
    {
        std::error_code ec;

        if (!fs::exists(path, ec) || ec)
            return false;

        uintmax_t currentSize =
            fs::file_size(path, ec);

        if (ec)
        {
            std::this_thread::sleep_for(
                std::chrono::seconds(1));
            continue;
        }

        if (currentSize > 44 && currentSize == lastSize)
            ++stableCount;
        else
            stableCount = 0;

        if (stableCount >= 2)
            return true;

        lastSize = currentSize;

        std::this_thread::sleep_for(
            std::chrono::seconds(1));
    }

    return false;
}

/**
 * @brief Load a WAV file, convert it to mono float samples, and resample to 16 kHz
 *
 * Logic:
 *   1. Open the WAV file using libsndfile; return false if the file cannot be opened;
 *   2. Validate the audio info (channels, samplerate, frames); close the file and return false if invalid;
 *   3. Allocate a buffer and read the raw multi-channel float samples using sf_readf_float;
 *   4. Close the file; return false if zero frames are read;
 *   5. Mix multi-channel audio down to mono by averaging the samples across all channels;
 *   6. If the original samplerate is already 16 kHz, skip resampling and return the mono samples directly;
 *   7. If resampling is needed, calculate the ratio, allocate an output buffer with extra capacity, and use libsamplerate (src_simple) to resample to 16 kHz;
 *   8. Handle resampling errors by clearing the output and returning false;
 *   9. Resize the output to the actual number of generated frames and return true if valid.
 *
 * @param filename Path to the input WAV file
 * @param samples  Output parameter to store the processed mono float samples at 16 kHz
 * @return true if the file is loaded and processed successfully; false otherwise
 * @note The output samples are always normalized to a single channel (mono) and
 *       resampled to WHISPER_SAMPLE_RATE (16000 Hz) as required by Whisper.
 */
bool loadWav(
    const std::string &filename,
    std::vector<float> &samples)
{
    SF_INFO info{};

    SNDFILE *file =
        sf_open(filename.c_str(), SFM_READ, &info);

    if (!file)
    {
        std::cerr << "[ERROR] can't open WAV: "
                  << filename << std::endl;

        return false;
    }

    if (info.channels <= 0 ||
        info.samplerate <= 0 ||
        info.frames <= 0)
    {
        sf_close(file);
        return false;
    }

    // Read raw audio data
    std::vector<float> raw(
        static_cast<size_t>(info.frames) *
        static_cast<size_t>(info.channels));

    sf_count_t framesRead =
        sf_readf_float(
            file,
            raw.data(),
            info.frames);

    sf_close(file);

    if (framesRead <= 0)
        return false;

    // Mix down multi-channel to mono
    std::vector<float> mono(
        static_cast<size_t>(framesRead));

    for (sf_count_t i = 0; i < framesRead; ++i)
    {
        float sum = 0.0f;

        for (int ch = 0; ch < info.channels; ++ch)
        {
            sum += raw[static_cast<size_t>(i) *
                           static_cast<size_t>(info.channels) +
                       static_cast<size_t>(ch)];
        }

        mono[static_cast<size_t>(i)] =
            sum / info.channels;
    }

    // 16 kHz, no resampling needed
    if (info.samplerate == WHISPER_SAMPLE_RATE)
    {
        samples = std::move(mono);

        return true;
    }

    // Target sample rate: 16000 Hz
    double ratio =
        static_cast<double>(WHISPER_SAMPLE_RATE) /
        static_cast<double>(info.samplerate);

    size_t outputCapacity =
        static_cast<size_t>(
            std::ceil(mono.size() * ratio)) +
        256;

    samples.resize(outputCapacity);

    SRC_DATA srcData{};

    srcData.data_in = mono.data();

    srcData.input_frames =
        static_cast<long>(mono.size());

    srcData.data_out = samples.data();

    srcData.output_frames =
        static_cast<long>(samples.size());

    srcData.src_ratio = ratio;

    srcData.end_of_input = 1;

    int error = src_simple(
        &srcData,
        SRC_SINC_BEST_QUALITY,
        1);

    if (error != 0)
    {
        std::cerr << "[ERROR] Sample error: "
                  << src_strerror(error) << std::endl;

        samples.clear();

        return false;
    }

    samples.resize(
        static_cast<size_t>(
            srcData.output_frames_gen));

    return !samples.empty();
}

/**
 * @brief Run Chinese speech recognition on the given wav file using whisper
 *
 * Workflow:
 *   1. Load the wav file into float samples; return false on failure;
 *   2. Initialize whisper parameters (greedy sampling, Chinese language, no translation,
 *      specified internal thread count);
 *   3. Disable context reuse and all progress/realtime/timestamp printing;
 *   4. Record the start time, call whisper_full to run recognition, and record the end time to calculate and print the inference duration;
 *   5. A non-zero return code from whisper_full is treated as failure;
 *   6. Iterate over all segments and concatenate their texts into the full result;
 *   7. Trim leading and trailing whitespace; set to an empty string if nothing remains.
 *
 * @param ctx      Pointer to the whisper context
 * @param filename Path to the wav file to recognize
 * @param result   Output parameter returning the recognized text
 * @return true on success; false if the wav fails to load or recognition fails
 * @note Language is fixed to Chinese (zh) with translation disabled; the result is only
 *       trimmed of leading/trailing whitespace, with no further cleaning.
 */
bool recognizeWav(
    whisper_context *ctx,
    const std::string &filename,
    std::string &result)
{
    std::vector<float> samples;

    if (!loadWav(filename, samples))
        return false;

    whisper_full_params params =
        whisper_full_default_params(
            WHISPER_SAMPLING_GREEDY);

    // Chinese speech recognition
    params.language = "zh";

    // Do not translate to English
    params.translate = false;

    // Whisper internal calculation threads
    params.n_threads = WHISPER_THREADS;

    params.no_context = true;

    params.print_progress = false;

    params.print_realtime = false;

    params.print_timestamps = false;

    std::cout << "[Recognize] Processing: "
              << filename << std::endl;

    // Record Whisper inference start time
    auto infer_start = std::chrono::steady_clock::now();

    int ret = whisper_full(
        ctx,
        params,
        samples.data(),
        static_cast<int>(samples.size()));

    // Record Whisper inference end time
    auto infer_end = std::chrono::steady_clock::now();

    double infer_time =
        std::chrono::duration<double>(
            infer_end - infer_start).count();

    std::cout << "[Performance] Whisper inference time: "
              << infer_time << " seconds"
              << std::endl;

    if (ret != 0)
    {
        std::cerr << "[Error] Recognition failed: "
                  << filename << std::endl;

        return false;
    }

    result.clear();

    int count = whisper_full_n_segments(ctx);

    for (int i = 0; i < count; ++i)
    {
        const char *text =
            whisper_full_get_segment_text(ctx, i);

        if (text)
            result += text;
    }

    // Remove leading and trailing whitespace
    size_t first =
        result.find_first_not_of(" \t\r\n");

    if (first == std::string::npos)
    {
        result.clear();
    }
    else
    {
        size_t last =
            result.find_last_not_of(" \t\r\n");

        result = result.substr(
            first,
            last - first + 1);
    }

    return true;
}

/**
 * @brief Append the recognition result of a single file to the result text file
 *
 * Workflow:
 *   1. Lock outputMutex to ensure mutually exclusive writes across threads;
 *   2. Open the result file in append mode; return false if opening fails;
 *   3. Write one line in the format "filename \t text \n";
 *   4. Flush the buffer and return the write status.
 *
 * @param filename Filename, used as the first column of the result line
 * @param text     Recognized text content
 * @return true on success; false if the file fails to open or the write fails
 * @note Protected by a mutex to prevent corruption of the result file under concurrent writes.
 */
bool appendResult(
    const std::string &filename,
    const std::string &text)
{
    std::lock_guard<std::mutex> lock(OutputMutex);

    std::ofstream out(RESULT_FILE,
                      std::ios::out | std::ios::app);

    if (!out.is_open())
    {
        std::cerr << "[ERROR] Result file not open\n";

        return false;
    }

    out << filename << "\t" << text << "\n";

    out.flush();

    return out.good();
}

/**
 * @brief Append a processed filename to the processing record file to persist processing state
 *
 * Workflow:
 *   1. Lock outputMutex to ensure mutually exclusive writes across threads;
 *   2. Open the record file in append mode; return false if opening fails;
 *   3. Write the filename followed by a newline;
 *   4. Flush the buffer and return the write status.
 *
 * @param filename Name of the file that has been fully processed
 * @return true on success; false if the file fails to open or the write fails
 * @note The record file is used to restore the processed set after a restart, avoiding duplicate recognition.
 */
bool saveProcessed(const std::string &filename)
{
    std::lock_guard<std::mutex> lock(OutputMutex);

    std::ofstream out;
    out.open(RECORD_FILE,
             std::ios::out | std::ios::app);
    if (!out.is_open())
    {
        std::cerr << "[ERROR] Output file not open\n";

        return false;
    }

    out << filename << std::endl;

    out.flush();

    return out.good();
}

/**
 * @brief Process a single wav file: stability check, speech recognition, result writing, and state recording
 *
 * Workflow:
 *   1. Extract the filename; return immediately if it is already in the processed set to avoid duplicate processing;
 *   2. Wait until the file is stable (fully written); print an error and return if not ready;
 *   3. Run whisper speech recognition; return if recognition fails;
 *   4. Append the recognition result to the TXT file; print an error and return on failure;
 *   5. Save the processing state record; print an error and return on failure;
 *   6. Insert the filename into the processed set and print a completion log.
 *
 * @param path      Path to the wav file to be processed
 * @param ctx       Pointer to the whisper context
 * @param processed Set of already processed filenames, used for deduplication
 * @return None
 * @note Any failure causes an early return; the filename is added to the processed set only after all steps succeed.
 */
void processFile(
    const fs::path &path,
    whisper_context *ctx,
    std::set<std::string> &processed)
{
    std::string filename = path.filename().string();

    // If this wav has already been processed, return
    if (processed.count(filename))
        return;

    // File not fully written; exit
    if (!waitForStableFile(path))
    {
        std::cerr << "[ERROR] File not ready:"
                  << filename << std::endl;

        return;
    }

    std::string result;

    if (!recognizeWav(ctx, path.string(), result))
    {
        std::cerr << "[ERROR] Recognition failed: "
                  << filename << std::endl;
        return;
    }

    // Append the obtained result to the file
    if (!appendResult(filename, result))
    {
        std::cerr << "[ERROR] TXT write failed: "
                  << filename << std::endl;

        return;
    }

    // Record the processed file
    if (!saveProcessed(filename))
    {
        std::cerr << "[ERROR] Failed to write processing record: "
                  << filename << std::endl;

        return;
    }
    processed.insert(filename);

    std::cout << "[COMPLETED] "
              << filename
              << " -> "
              << result
              << std::endl;
}

/**
 * @brief Recognition worker thread: pops wav file paths from the task queue
 *
 * Workflow:
 *   1. Acquires the queue mutex and waits on the condition variable;
 *   2. Sleeps when the queue is empty (and running is true), consuming no CPU;
 *   3. Wakes up when the main thread enqueues a new task or running becomes false;
 *   4. Pops the front path from the queue;
 *   5. Exits the loop when running is false and the queue is empty;
 *   6. Prints an exit message when the thread terminates.
 *
 * @param ctx       Pointer to the whisper context (currently unused in the loop body)
 * @param processed Set of processed filenames (passed by value; modifications are local only)
 * @return None
 * @note Uses a condition variable to sleep when the queue is empty and wake up when tasks arrive.
 *       TODO: the popped path is not yet passed to processFile(); recognition logic is missing.
 */
void recognitionWorker(
    whisper_context *ctx,
    std::set<std::string> &processed)
{
    while (true)
    {
        fs::path path;
        {
            std::unique_lock<std::mutex> lock(queueMutex);

            queueCV.wait(lock,
                         []
                         {
                             return !TaskQueue.empty() ||
                                    !running.load();
                         });
            if (!running && TaskQueue.empty())
                break;

            path = TaskQueue.top();

            TaskQueue.pop();
        }
        processFile(path,
                    ctx,
                    processed);
    }

    std::cout << "[THREAD] Recognition thread exits\n";
}

/**
 * @brief Check if the given file path has a WAV extension (case-insensitive)
 *
 * Logic:
 *   1. Extract the file extension from the given path;
 *   2. Convert the extension to lowercase using std::transform and std::tolower;
 *   3. Compare the normalized extension with ".wav" and return the result.
 *
 * @param path The file path to check
 * @return true if the file has a ".wav" extension (case-insensitive); false otherwise
 */
bool isWav(const fs::path &path)
{
    std::string ext = path.extension().string();

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

    return ext == ".wav";
}

/**
 * @brief Validate and enqueue a new WAV file into the processing task queue
 *
 * Logic:
 *   1. Check if the file has a .wav extension; return early if not;
 *   2. Check if the path points to a regular file; return early on error or if not a regular file;
 *   3. Acquire the mutex lock to safely access shared data;
 *   4. Check if the file is already scheduled; return early if it is (to avoid duplicate processing);
 *   5. Insert the filename into the scheduled set and push the path into the task queue;
 *   6. Release the lock, print the discovery message to stdout, and notify one waiting worker thread via condition variable.
 *
 * @param path The file path of the newly discovered file
 * @note This function is thread-safe regarding the queue and the scheduled set due to the mutex lock.
 */
void enqueueFile(const fs::path &path)
{
    if (!isWav(path))
        return;

    std::error_code ec;

    if (!fs::is_regular_file(path, ec) || ec)
        return;

    std::string filename = path.filename().string();

    {
        std::lock_guard<std::mutex> lock(queueMutex);

        if (scheduledFiles.count(filename))
            return;

        scheduledFiles.insert(filename);

        TaskQueue.push(path);
    }

    std::cout << "[Monitor] New file found: "
              << filename << std::endl;

    queueCV.notify_one();
}

/**
 * @brief Entry point of the WAV automatic speech recognition system
 *
 * Workflow:
 *   1. Register SIGINT and SIGTERM signal handlers for graceful shutdown;
 *   2. Create necessary input and output directories if they do not exist;
 *   3. Load the Whisper model into memory; exit if loading fails;
 *   4. Load historical processed records and initialize the scheduled files set;
 *   5. Initialize inotify to watch the input directory for new files (IN_CLOSE_WRITE | IN_MOVED_TO);
 *   6. Scan existing WAV files in the input directory, sort them chronologically by timestamp, and enqueue them;
 *   7. Spawn the background recognition worker thread;
 *   8. Enter the main loop: use poll() to wait for inotify events, parse the buffer, and enqueue newly arrived WAV files;
 *   9. On exit (running == false), notify all threads, join the worker thread, and clean up resources (inotify, Whisper context).
 *
 * @return 0 on successful execution; 1 if critical initialization (model loading, inotify) fails
 * @note The program relies on inotify for real-time file monitoring and uses a multi-threaded
 *       architecture: the main thread for event listening and the worker thread for ASR processing.
 */
int main()
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    // Create output dir
    fs::create_directories(INPUT_DIR);
    fs::create_directories(OUTPUT_DIR);

    // Load whisper module
    std::cout << "Loading Whisper module....\n";

    whisper_context_params cparams =
        whisper_context_default_params();

    whisper_context *ctx =
        whisper_init_from_file_with_params(
            MODEL_FILE.c_str(),
            cparams);
    if (!ctx)
    {
        std::cerr << "[[ERROR]] Module load error";

        return 1;
    }
    std::cout << "[SUCCEED] Module load success";

    // Load historical records
    auto processed = LoadProcessed();
    scheduledFiles = processed;

    // Initialize inotify
    int fd = inotify_init1(IN_NONBLOCK);

    if (fd < 0)
    {
        perror("Inotify init error\n");

        whisper_free(ctx);

        return 1;
    }

    // Watch the directory
    int watch = inotify_add_watch(fd, INPUT_DIR.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO);

    if (watch < 0)
    {
        perror("Inotify_add_watch error\n");

        close(fd);

        whisper_free(ctx);

        return 1;
    }

    // =====================================================
    // Scan existing WAV files
    // =====================================================

    std::vector<fs::path> existingFiles;

    for (const auto &entry :
         fs::directory_iterator(INPUT_DIR))
    {
        if (entry.is_regular_file() && isWav(entry.path()))
        {
            existingFiles.push_back(entry.path());
        }
    }

    // Sort in ascending order by timestamp
    std::sort(
        existingFiles.begin(),
        existingFiles.end(),
        [](const fs::path &a, const fs::path &b)
        {
            std::chrono::system_clock::time_point timeA;
            std::chrono::system_clock::time_point timeB;

            bool validA = getTimestamp(a, timeA);
            bool validB = getTimestamp(b, timeB);

            if (validA != validB)
                return validA;

            if (!validA)
                return a.filename().string() <
                       b.filename().string();

            if (timeA != timeB)
                return timeA < timeB;

            return a.filename().string() <
                   b.filename().string();
        });

    // Enqueue in chronological order
    for (const auto &path : existingFiles)
    {
        enqueueFile(path);
    }

    // After all existing files are enqueued, start the worker
    std::thread worker(
        recognitionWorker,
        ctx,
        std::ref(processed));

    std::cout << "\n================================\n";

    std::cout << "WAV Automatic Speech Recognition System Started\n";

    std::cout << "Watching directory: " << INPUT_DIR << "\n";

    std::cout << "Result file: " << RESULT_FILE << "\n";

    std::cout << "Press Ctrl+C to exit\n";

    std::cout << "================================\n";

    // inotify event buffer
    alignas(struct inotify_event) char buffer[4096];

    // Main thread continuously listens for events
    while (running)
    {
        pollfd pfd{};

        pfd.fd = fd;

        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000);

        if (ret < 0)
        {
            if (!running)
                break;

            continue;
        }

        if (ret == 0)
            continue;

        ssize_t len =
            read(fd, buffer, sizeof(buffer));

        if (len <= 0)
            continue;

        ssize_t offset = 0;

        while (offset < len)
        {
            auto *event =
                reinterpret_cast<struct inotify_event *>(
                    buffer + offset);

            // Process files only, ignore directories
            if (!(event->mask & IN_ISDIR) &&
                event->len > 0 &&
                (event->mask &
                 (IN_CLOSE_WRITE | IN_MOVED_TO)))
            {
                fs::path path =
                    fs::path(INPUT_DIR) / event->name;

                enqueueFile(path);
            }

            offset +=
                sizeof(struct inotify_event) +
                event->len;
        }
    }

    // Notify the worker thread
    queueCV.notify_all();

    // Wait for the recognition thread to exit
    worker.join();

    // Release resources
    inotify_rm_watch(fd, watch);

    close(fd);

    whisper_free(ctx);

    std::cout << "\nProgram safely exited."
              << std::endl;

    return 0;
}
