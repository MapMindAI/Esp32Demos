#ifndef APP_CONFIG_H
#define APP_CONFIG_H

// Sparse selector / small-image pipeline configuration.
// Downsample ratio for selector input image (2 => width/2,height/2 => 1/4 pixels).
#define SELECTOR_SMALL_IMG_SCALE 2
// Enable dual-core parallel selector processing (top/bottom split) when SMP is available.
#define SELECTOR_USE_DUAL_CORE 1
// Histogram tile size in pixels on the small image.
#define SELECTOR_BLOCK_SIZE 32
// Selection grid step. Larger value => fewer selected pixels.
#define SELECTOR_POTENTIAL 6
// Additive histogram threshold bias. Larger value => stricter gradient gate.
#define SELECTOR_GRAD_HIST_ADD 10
// Multiplicative scale on local smoothed threshold. Larger => stricter selection.
#define SELECTOR_THRESHOLD_SCALE 1.6f
// Minimum directional response (best_val) for accepting one selected pixel.
#define SELECTOR_BEST_VAL_MIN 30.0f

// Debug log interval in frames (selector timing and first few selected points).
#define SELECTOR_LOG_INTERVAL 30

#endif
