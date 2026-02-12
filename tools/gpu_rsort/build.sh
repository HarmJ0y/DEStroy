#!/bin/bash
# Rainbow Table GPU Sorter - Helper Script

set -e

echo "=========================================="
echo "GPU Rainbow Table Sorter Setup"
echo "=========================================="

# Check for CUDA
if ! command -v nvcc &> /dev/null; then
    echo "ERROR: nvcc not found. Install CUDA toolkit first."
    echo "  Ubuntu: sudo apt install nvidia-cuda-toolkit"
    exit 1
fi

echo "CUDA compiler found: $(nvcc --version | head -1)"
echo ""

# Compile
echo "Compiling rtsort_gpu..."
make clean 2>/dev/null || true
make

echo ""
echo "Build successful!"
echo ""

# Print usage
echo "=========================================="
echo "Usage Examples"
echo "=========================================="
echo ""
echo "1. Sort a single file:"
echo "   ./rtsort_gpu /path/to/table.rt"
echo ""
echo "2. Sort all tables in a directory:"
echo "   ./rtsort_gpu /path/to/rainbow_tables/"
echo ""
echo "3. Use more worker threads (for I/O-bound workloads):"
echo "   ./rtsort_gpu /path/to/tables/ --threads 4"
echo ""
echo "4. Dry run (see what would be sorted):"
echo "   ./rtsort_gpu /path/to/tables/ --dry-run"
echo ""

# Performance estimates
echo "=========================================="
echo "Performance Estimates (4x L4 GPUs)"
echo "=========================================="
echo ""
echo "Per-file (2GB table):"
echo "  - GPU sort time:     ~1-2 seconds"
echo "  - Memory transfer:   ~2-3 seconds (H2D + D2H)"
echo "  - Disk I/O:          ~5-15 seconds (depends on storage)"
echo "  - Total per file:    ~10-20 seconds"
echo ""
echo "Full workload (4096 x 2GB = 8TB):"
echo "  - With NVMe storage: ~3-6 hours"
echo "  - With SATA SSD:     ~8-12 hours"  
echo "  - With HDD:          ~24-48 hours"
echo ""
echo "Tips for maximum throughput:"
echo "  1. Use NVMe storage if possible"
echo "  2. Increase --threads if I/O is the bottleneck"
echo "  3. Split files across multiple drives"
echo "  4. Monitor with: watch -n1 nvidia-smi"
echo ""
