# Transformed from Python to Go.

Original implementation: [OpenCVProjects/laplacian_variance_blur_detection.ipynb](https://github.com/behnamasadi/OpenCVProjects/blob/master/docs/laplacian_variance_blur_detection.ipynb)

### Optimizations

* **Pre-allocation & Buffer Reuse:** Allocates memory once at startup instead of per frame, achieving `0 B/op` and `0 allocs/op`.
* **Rolling Buffer:** Computes the $3 \times 3$ Laplacian filter across a rolling 3-row buffer, avoiding new allocations during the hot path.
* **Welford's Algorithm:** Calculates running mean and variance in a single pass with $O(1)$ auxiliary memory.

### References

* [Go SliceTricks](https://go.dev/wiki/SliceTricks)
* [Circular Buffer (Wikipedia)](https://en.wikipedia.org/wiki/Circular_buffer)
* [Kernel / Image Processing (Wikipedia)](https://en.wikipedia.org/wiki/Kernel_(image_processing))
* [Go Interfaces (Russ Cox)](https://research.swtch.com/interfaces)
* [Algorithms for calculating variance (Wikipedia)](https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance)
