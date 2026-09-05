Transformed from Python to Golang

[Source](https://github.com/behnamasadi/OpenCVProjects/blob/master/docs/laplacian_variance_blur_detection.ipynb)

Optimized: Pre-allocation & Buffer Reuse instead of each frame alloc so 0 allocs/op, laplacian filter using rolling buffer so no new alloc during hot path, use Welford algorithm so it calculates a running mean and variance in a single pass with O(1) constant memory.

thats all !

ref: 
https://go.dev/wiki/SliceTricks
https://en.wikipedia.org/wiki/Circular_buffer
https://en.wikipedia.org/wiki/Kernel_(image_processing)
https://research.swtch.com/interfaces
https://en.wikipedia.org/wiki/Algorithms_for_calculating_variance
