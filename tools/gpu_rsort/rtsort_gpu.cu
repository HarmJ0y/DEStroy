/*
 * GPU-Accelerated Rainbow Table Sorter
 * 
 * Sorts rainbow tables using CUDA Thrust radix sort across multiple GPUs.
 * Processes multiple files in parallel for maximum throughput.
 * 
 * Compile: nvcc -O3 -o rtsort_gpu rtsort_gpu.cu -lpthread
 * Usage:   ./rtsort_gpu <file_or_directory> [--threads N] [--dry-run]
 * 
 * Example: ./rtsort_gpu /path/to/tables/ --threads 8
 *          ./rtsort_gpu single_table.rt
 */

#include <thrust/device_vector.h>
#include <thrust/sort.h>
#include <thrust/execution_policy.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

// Rainbow chain structure - 16 bytes total
struct RainbowChain {
    uint64_t nIndexS;  // Start index
    uint64_t nIndexE;  // End index (sort key)
};

static_assert(sizeof(RainbowChain) == 16, "RainbowChain must be 16 bytes");

// Comparison functor for sorting by end index
struct ChainCompare {
    __host__ __device__
    bool operator()(const RainbowChain& a, const RainbowChain& b) const {
        return a.nIndexE < b.nIndexE;
    }
};

// Global statistics
struct Stats {
    std::atomic<size_t> files_processed{0};
    std::atomic<size_t> files_failed{0};
    std::atomic<size_t> bytes_processed{0};
    std::atomic<size_t> total_files{0};
    std::chrono::steady_clock::time_point start_time;
    std::mutex print_mutex;
};

Stats g_stats;

// Thread-safe print
template<typename... Args>
void safe_print(const char* fmt, Args... args) {
    std::lock_guard<std::mutex> lock(g_stats.print_mutex);
    printf(fmt, args...);
    fflush(stdout);
}

// Get file size
size_t get_file_size(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_size;
}

// Check if file is already sorted (quick heuristic check)
bool is_likely_sorted(const std::string& path, size_t file_size) {
    if (file_size < 1024 * 16) return false;  // Too small to bother checking
    
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    
    // Check first 100 and last 100 chains
    const int sample_size = 100;
    std::vector<RainbowChain> first_chains(sample_size);
    std::vector<RainbowChain> last_chains(sample_size);
    
    // Read first chains
    if (fread(first_chains.data(), sizeof(RainbowChain), sample_size, f) != sample_size) {
        fclose(f);
        return false;
    }
    
    // Read last chains
    fseek(f, -(long)(sample_size * sizeof(RainbowChain)), SEEK_END);
    if (fread(last_chains.data(), sizeof(RainbowChain), sample_size, f) != sample_size) {
        fclose(f);
        return false;
    }
    fclose(f);
    
    // Check if samples are sorted
    for (int i = 1; i < sample_size; i++) {
        if (first_chains[i].nIndexE < first_chains[i-1].nIndexE) return false;
        if (last_chains[i].nIndexE < last_chains[i-1].nIndexE) return false;
    }
    
    // Check if last of first < first of last (approximate global order)
    if (last_chains[0].nIndexE < first_chains[sample_size-1].nIndexE) return false;
    
    return true;
}

// Sort a single file using specified GPU
bool sort_file_gpu(const std::string& path, int gpu_id, bool dry_run) {
    auto file_start = std::chrono::steady_clock::now();
    
    // Set GPU
    cudaError_t err = cudaSetDevice(gpu_id);
    if (err != cudaSuccess) {
        safe_print("[GPU %d] ERROR: Failed to set device: %s\n", gpu_id, cudaGetErrorString(err));
        return false;
    }
    
    // Get file size
    size_t file_size = get_file_size(path);
    if (file_size == 0) {
        safe_print("[GPU %d] ERROR: Cannot read file %s\n", gpu_id, path.c_str());
        return false;
    }
    
    if (file_size % sizeof(RainbowChain) != 0) {
        safe_print("[GPU %d] ERROR: Invalid file size for %s (not multiple of 16)\n", gpu_id, path.c_str());
        return false;
    }
    
    size_t chain_count = file_size / sizeof(RainbowChain);
    
    // Quick sorted check
    if (is_likely_sorted(path, file_size)) {
        safe_print("[GPU %d] SKIP: %s appears already sorted\n", gpu_id, path.c_str());
        g_stats.files_processed++;
        g_stats.bytes_processed += file_size;
        return true;
    }
    
    if (dry_run) {
        safe_print("[GPU %d] DRY-RUN: Would sort %s (%.2f GB, %zu chains)\n", 
                   gpu_id, path.c_str(), file_size / (1024.0*1024.0*1024.0), chain_count);
        g_stats.files_processed++;
        return true;
    }
    
    safe_print("[GPU %d] START: %s (%.2f GB, %zu chains)\n", 
               gpu_id, path.c_str(), file_size / (1024.0*1024.0*1024.0), chain_count);
    
    // Allocate pinned host memory for faster transfers
    RainbowChain* h_chains;
    err = cudaMallocHost(&h_chains, file_size);
    if (err != cudaSuccess) {
        safe_print("[GPU %d] ERROR: Failed to allocate pinned memory: %s\n", gpu_id, cudaGetErrorString(err));
        return false;
    }
    
    // Read file
    auto io_start = std::chrono::steady_clock::now();
    FILE* f = fopen(path.c_str(), "r+b");
    if (!f) {
        safe_print("[GPU %d] ERROR: Cannot open file %s\n", gpu_id, path.c_str());
        cudaFreeHost(h_chains);
        return false;
    }
    
    size_t bytes_read = fread(h_chains, 1, file_size, f);
    if (bytes_read != file_size) {
        safe_print("[GPU %d] ERROR: Read failed for %s (got %zu, expected %zu)\n", 
                   gpu_id, path.c_str(), bytes_read, file_size);
        fclose(f);
        cudaFreeHost(h_chains);
        return false;
    }
    auto io_read_time = std::chrono::steady_clock::now() - io_start;
    
    // Allocate device memory
    RainbowChain* d_chains;
    err = cudaMalloc(&d_chains, file_size);
    if (err != cudaSuccess) {
        safe_print("[GPU %d] ERROR: Failed to allocate device memory: %s\n", gpu_id, cudaGetErrorString(err));
        fclose(f);
        cudaFreeHost(h_chains);
        return false;
    }
    
    // Copy to GPU
    auto transfer_start = std::chrono::steady_clock::now();
    err = cudaMemcpy(d_chains, h_chains, file_size, cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
        safe_print("[GPU %d] ERROR: H2D transfer failed: %s\n", gpu_id, cudaGetErrorString(err));
        cudaFree(d_chains);
        fclose(f);
        cudaFreeHost(h_chains);
        return false;
    }
    auto h2d_time = std::chrono::steady_clock::now() - transfer_start;
    
    // Sort on GPU using Thrust
    auto sort_start = std::chrono::steady_clock::now();
    try {
        thrust::sort(thrust::device, d_chains, d_chains + chain_count, ChainCompare());
        cudaDeviceSynchronize();
    } catch (const thrust::system_error& e) {
        safe_print("[GPU %d] ERROR: Thrust sort failed: %s\n", gpu_id, e.what());
        cudaFree(d_chains);
        fclose(f);
        cudaFreeHost(h_chains);
        return false;
    }
    auto sort_time = std::chrono::steady_clock::now() - sort_start;
    
    // Copy back to host
    transfer_start = std::chrono::steady_clock::now();
    err = cudaMemcpy(h_chains, d_chains, file_size, cudaMemcpyDeviceToHost);
    if (err != cudaSuccess) {
        safe_print("[GPU %d] ERROR: D2H transfer failed: %s\n", gpu_id, cudaGetErrorString(err));
        cudaFree(d_chains);
        fclose(f);
        cudaFreeHost(h_chains);
        return false;
    }
    auto d2h_time = std::chrono::steady_clock::now() - transfer_start;
    
    // Free device memory early
    cudaFree(d_chains);
    
    // Write back to file
    io_start = std::chrono::steady_clock::now();
    fseek(f, 0, SEEK_SET);
    size_t bytes_written = fwrite(h_chains, 1, file_size, f);
    fflush(f);
    fclose(f);
    auto io_write_time = std::chrono::steady_clock::now() - io_start;
    
    cudaFreeHost(h_chains);
    
    if (bytes_written != file_size) {
        safe_print("[GPU %d] ERROR: Write failed for %s\n", gpu_id, path.c_str());
        return false;
    }
    
    auto total_time = std::chrono::steady_clock::now() - file_start;
    
    double read_ms = std::chrono::duration<double, std::milli>(io_read_time).count();
    double h2d_ms = std::chrono::duration<double, std::milli>(h2d_time).count();
    double sort_ms = std::chrono::duration<double, std::milli>(sort_time).count();
    double d2h_ms = std::chrono::duration<double, std::milli>(d2h_time).count();
    double write_ms = std::chrono::duration<double, std::milli>(io_write_time).count();
    double total_ms = std::chrono::duration<double, std::milli>(total_time).count();
    
    double throughput_gbs = (file_size / (1024.0*1024.0*1024.0)) / (total_ms / 1000.0);
    
    safe_print("[GPU %d] DONE: %s in %.1fs (read:%.1f h2d:%.1f sort:%.1f d2h:%.1f write:%.1fs) %.2f GB/s\n",
               gpu_id, fs::path(path).filename().c_str(), total_ms/1000.0,
               read_ms/1000.0, h2d_ms/1000.0, sort_ms/1000.0, d2h_ms/1000.0, write_ms/1000.0,
               throughput_gbs);
    
    g_stats.files_processed++;
    g_stats.bytes_processed += file_size;
    
    return true;
}

// Thread-safe work queue
class WorkQueue {
public:
    void push(const std::string& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(item);
        cv_.notify_one();
    }
    
    bool pop(std::string& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }
    
    void set_done() {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        cv_.notify_all();
    }
    
    size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }
    
private:
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool done_ = false;
};

// Worker thread function
void worker_thread(WorkQueue& queue, int gpu_id, bool dry_run) {
    std::string file;
    while (queue.pop(file)) {
        if (!sort_file_gpu(file, gpu_id, dry_run)) {
            g_stats.files_failed++;
        }
    }
}

// Find all rainbow table files
std::vector<std::string> find_rainbow_tables(const std::string& path) {
    std::vector<std::string> files;
    
    if (fs::is_regular_file(path)) {
        files.push_back(path);
        return files;
    }
    
    if (!fs::is_directory(path)) {
        return files;
    }
    
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        // Common rainbow table extensions
        if (ext == ".rt" || ext == ".rtc" || ext == ".rti") {
            size_t size = entry.file_size();
            if (size > 0 && size % 16 == 0) {
                files.push_back(entry.path().string());
            }
        }
    }
    
    // Sort by size descending (process largest files first for better load balancing)
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return get_file_size(a) > get_file_size(b);
    });
    
    return files;
}

void print_progress() {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - g_stats.start_time).count();
    size_t processed = g_stats.files_processed.load();
    size_t total = g_stats.total_files.load();
    size_t bytes = g_stats.bytes_processed.load();
    
    double progress = total > 0 ? (100.0 * processed / total) : 0;
    double throughput = elapsed > 0 ? (bytes / (1024.0*1024.0*1024.0)) / elapsed : 0;
    
    double eta = 0;
    if (processed > 0 && processed < total) {
        eta = (elapsed / processed) * (total - processed);
    }
    
    safe_print("\n=== Progress: %zu/%zu files (%.1f%%) | %.2f GB processed | %.2f GB/s | ETA: %.0fs ===\n\n",
               processed, total, progress, bytes / (1024.0*1024.0*1024.0), throughput, eta);
}

void print_usage(const char* prog) {
    printf("GPU-Accelerated Rainbow Table Sorter\n\n");
    printf("Usage: %s <path> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --threads N    Number of worker threads per GPU (default: 2)\n");
    printf("  --dry-run      Show what would be sorted without actually sorting\n");
    printf("  --help         Show this help\n\n");
    printf("Examples:\n");
    printf("  %s /path/to/tables/           # Sort all .rt files in directory\n", prog);
    printf("  %s single_table.rt            # Sort a single file\n", prog);
    printf("  %s /path/to/tables/ --threads 4   # Use 4 threads per GPU\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string path;
    int threads_per_gpu = 2;
    bool dry_run = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads_per_gpu = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }
    
    if (path.empty()) {
        fprintf(stderr, "Error: No path specified\n");
        return 1;
    }
    
    // Get GPU count
    int gpu_count;
    cudaGetDeviceCount(&gpu_count);
    if (gpu_count == 0) {
        fprintf(stderr, "Error: No CUDA-capable GPUs found\n");
        return 1;
    }
    
    printf("Found %d GPU(s):\n", gpu_count);
    for (int i = 0; i < gpu_count; i++) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);
        printf("  [%d] %s (%.1f GB, %d SMs)\n", 
               i, prop.name, prop.totalGlobalMem / (1024.0*1024.0*1024.0), prop.multiProcessorCount);
    }
    printf("\n");
    
    // Find files
    printf("Scanning for rainbow tables in: %s\n", path.c_str());
    std::vector<std::string> files = find_rainbow_tables(path);
    
    if (files.empty()) {
        fprintf(stderr, "No rainbow table files found (.rt, .rtc, .rti)\n");
        return 1;
    }
    
    // Calculate total size
    size_t total_size = 0;
    for (const auto& f : files) {
        total_size += get_file_size(f);
    }
    
    printf("Found %zu files (%.2f GB total)\n", files.size(), total_size / (1024.0*1024.0*1024.0));
    printf("Using %d threads per GPU (%d total threads)\n\n", threads_per_gpu, threads_per_gpu * gpu_count);
    
    if (dry_run) {
        printf("=== DRY RUN MODE - No files will be modified ===\n\n");
    }
    
    g_stats.total_files = files.size();
    g_stats.start_time = std::chrono::steady_clock::now();
    
    // Create work queue and add files
    WorkQueue queue;
    for (const auto& f : files) {
        queue.push(f);
    }
    
    // Create worker threads
    std::vector<std::thread> workers;
    int total_threads = threads_per_gpu * gpu_count;
    
    for (int i = 0; i < total_threads; i++) {
        int gpu_id = i % gpu_count;
        workers.emplace_back(worker_thread, std::ref(queue), gpu_id, dry_run);
    }
    
    // Progress reporting thread
    std::atomic<bool> progress_done{false};
    std::thread progress_thread([&]() {
        while (!progress_done) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!progress_done) {
                print_progress();
            }
        }
    });
    
    // Signal queue completion
    queue.set_done();
    
    // Wait for workers
    for (auto& t : workers) {
        t.join();
    }
    
    progress_done = true;
    progress_thread.join();
    
    // Final stats
    auto total_time = std::chrono::steady_clock::now() - g_stats.start_time;
    double elapsed = std::chrono::duration<double>(total_time).count();
    double throughput = g_stats.bytes_processed / (1024.0*1024.0*1024.0) / elapsed;
    
    printf("\n========================================\n");
    printf("Completed in %.1f seconds\n", elapsed);
    printf("Files processed: %zu\n", g_stats.files_processed.load());
    printf("Files failed: %zu\n", g_stats.files_failed.load());
    printf("Data processed: %.2f GB\n", g_stats.bytes_processed / (1024.0*1024.0*1024.0));
    printf("Average throughput: %.2f GB/s\n", throughput);
    printf("========================================\n");
    
    return g_stats.files_failed > 0 ? 1 : 0;
}
