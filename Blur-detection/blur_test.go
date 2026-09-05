package main

import (
	"bytes"
	"image"
	"image/color"
	"image/png"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestUniformImageHasZeroVariance(t *testing.T) {
	img := image.NewGray(image.Rect(0, 0, 16, 16))
	for i := range img.Pix {
		img.Pix[i] = 127
	}

	result, err := Analyze(img, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze returned an error: %v", err)
	}
	if result.Score != 0 {
		t.Fatalf("uniform image score = %v, want 0", result.Score)
	}
	if result.Status != StatusCheck {
		t.Fatalf("uniform image status = %q, want %q", result.Status, StatusCheck)
	}
}

func TestBlurReducesVariance(t *testing.T) {
	sharp := verticalStripes(64, 64, 4)
	blurred := boxBlur(sharp, 2)

	sharpResult, err := Analyze(sharp, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze(sharp) returned an error: %v", err)
	}
	blurredResult, err := Analyze(blurred, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze(blurred) returned an error: %v", err)
	}
	if blurredResult.Score >= sharpResult.Score {
		t.Fatalf(
			"blurred score (%v) should be less than sharp score (%v)",
			blurredResult.Score,
			sharpResult.Score,
		)
	}
}

func TestClassifyBoundaries(t *testing.T) {
	tests := []struct {
		score float64
		want  Status
	}{
		{150, StatusNormal},
		{149.99, StatusWarning},
		{100, StatusWarning},
		{99.99, StatusRisk},
		{50, StatusRisk},
		{49.99, StatusCheck},
	}

	for _, test := range tests {
		if got := Classify(test.score, DefaultThresholds); got != test.want {
			t.Errorf("Classify(%v) = %q, want %q", test.score, got, test.want)
		}
	}
}

func TestGray16PreservesDynamicRange(t *testing.T) {
	gray8 := image.NewGray(image.Rect(0, 0, 12, 12))
	gray16 := image.NewGray16(image.Rect(0, 0, 12, 12))
	for y := 0; y < 12; y++ {
		for x := 0; x < 12; x++ {
			value := uint8(32)
			if x >= 6 {
				value = 224
			}
			gray8.SetGray(x, y, color.Gray{Y: value})
			gray16.SetGray16(x, y, color.Gray16{Y: uint16(value) * 257})
		}
	}

	result8, err := Analyze(gray8, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze(gray8) returned an error: %v", err)
	}
	result16, err := Analyze(gray16, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze(gray16) returned an error: %v", err)
	}

	wantRatio := float64(257 * 257)
	gotRatio := result16.Score / result8.Score
	if math.Abs(gotRatio-wantRatio) > 1e-6 {
		t.Fatalf("16-bit/8-bit variance ratio = %v, want %v", gotRatio, wantRatio)
	}
	if result16.BitDepth != 16 || result8.BitDepth != 8 {
		t.Fatalf("reported bit depths = %d and %d, want 16 and 8", result16.BitDepth, result8.BitDepth)
	}
}

func TestAnalyzeSupportsNonZeroImageOrigin(t *testing.T) {
	img := image.NewRGBA(image.Rect(10, 20, 26, 36))
	for y := img.Bounds().Min.Y; y < img.Bounds().Max.Y; y++ {
		for x := img.Bounds().Min.X; x < img.Bounds().Max.X; x++ {
			value := uint8(0)
			if x >= 18 {
				value = 255
			}
			img.SetRGBA(x, y, color.RGBA{R: value, G: value, B: value, A: 255})
		}
	}

	result, err := Analyze(img, DefaultThresholds)
	if err != nil {
		t.Fatalf("Analyze returned an error: %v", err)
	}
	if result.Width != 16 || result.Height != 16 {
		t.Fatalf("size = %dx%d, want 16x16", result.Width, result.Height)
	}
	if result.Score <= 0 {
		t.Fatalf("edge image score = %v, want a positive value", result.Score)
	}
}

func TestThresholdValidation(t *testing.T) {
	invalid := []Thresholds{
		{Warning: 100, Risk: 100, Check: 50},
		{Warning: 50, Risk: 100, Check: 10},
		{Warning: 100, Risk: 50, Check: -1},
		{Warning: math.NaN(), Risk: 100, Check: 50},
	}
	for _, thresholds := range invalid {
		if err := thresholds.Validate(); err == nil {
			t.Errorf("Validate(%+v) returned nil, want an error", thresholds)
		}
	}
}

func TestDetectorCapacityIncludesReserveAndAlignment(t *testing.T) {
	detector, err := NewDetector(DetectorConfig{
		ExpectedWidth:  2048,
		ReservePercent: 15,
		Thresholds:     DefaultThresholds,
	})
	if err != nil {
		t.Fatalf("NewDetector returned an error: %v", err)
	}
	if detector.CapacityWidth() != 2368 {
		t.Fatalf("capacity = %d, want 2368", detector.CapacityWidth())
	}
	if detector.BufferBytes() != 2368*3*2 {
		t.Fatalf("buffer bytes = %d, want %d", detector.BufferBytes(), 2368*3*2)
	}
}

func TestDetectorRejectsOversizedFrameWithoutGrowing(t *testing.T) {
	detector, err := NewDetector(DetectorConfig{
		ExpectedWidth:  64,
		ReservePercent: 0,
		Thresholds:     DefaultThresholds,
	})
	if err != nil {
		t.Fatalf("NewDetector returned an error: %v", err)
	}
	before := detector.BufferBytes()
	_, err = detector.Analyze(image.NewGray(image.Rect(0, 0, 65, 8)))
	if err == nil {
		t.Fatal("Analyze returned nil error for an oversized frame")
	}
	if detector.BufferBytes() != before {
		t.Fatalf("buffer grew from %d to %d bytes", before, detector.BufferBytes())
	}
}

func TestDetectorMatchesAllocatingBaseline(t *testing.T) {
	img := verticalStripes(67, 49, 5)
	detector, err := NewDetector(DetectorConfig{
		ExpectedWidth:  img.Bounds().Dx(),
		ReservePercent: 20,
		Thresholds:     DefaultThresholds,
	})
	if err != nil {
		t.Fatalf("NewDetector returned an error: %v", err)
	}

	got, err := detector.Analyze(img)
	if err != nil {
		t.Fatalf("Detector.Analyze returned an error: %v", err)
	}
	want := analyzeAllocatingBaseline(img, DefaultThresholds)
	if got.Score != want.Score {
		t.Fatalf("rolling score = %.12f, allocating score = %.12f", got.Score, want.Score)
	}
}

func TestDetectorHotPathHasNoAllocations(t *testing.T) {
	images := []struct {
		name string
		img  image.Image
	}{
		{"Gray", verticalStripes(128, 96, 4)},
		{"Gray16", image.NewGray16(image.Rect(0, 0, 128, 96))},
		{"YCbCrLikeDecodedJPEG", ycbcrStripes(128, 96, 4)},
		{"NYCbCrA", image.NewNYCbCrA(image.Rect(0, 0, 128, 96), image.YCbCrSubsampleRatio420)},
		{"RGBA", image.NewRGBA(image.Rect(0, 0, 128, 96))},
		{"NRGBA", image.NewNRGBA(image.Rect(0, 0, 128, 96))},
		{"RGBA64", image.NewRGBA64(image.Rect(0, 0, 128, 96))},
		{"NRGBA64", image.NewNRGBA64(image.Rect(0, 0, 128, 96))},
		{"CMYK", image.NewCMYK(image.Rect(0, 0, 128, 96))},
		{"Paletted", image.NewPaletted(image.Rect(0, 0, 128, 96), color.Palette{color.Black, color.White})},
	}

	for _, test := range images {
		t.Run(test.name, func(t *testing.T) {
			detector, err := NewDetector(DetectorConfig{
				ExpectedWidth:  128,
				ReservePercent: 15,
				Thresholds:     DefaultThresholds,
			})
			if err != nil {
				t.Fatalf("NewDetector returned an error: %v", err)
			}
			if _, err := detector.Analyze(test.img); err != nil {
				t.Fatalf("warm-up Analyze returned an error: %v", err)
			}

			var analyzeErr error
			allocations := testing.AllocsPerRun(100, func() {
				_, analyzeErr = detector.Analyze(test.img)
			})
			if analyzeErr != nil {
				t.Fatalf("Analyze returned an error: %v", analyzeErr)
			}
			if allocations != 0 {
				t.Fatalf("hot-path allocations = %v, want 0", allocations)
			}
		})
	}
}

func TestRunReadsPNGAndPrintsResult(t *testing.T) {
	img := verticalStripes(24, 24, 3)
	path := filepath.Join(t.TempDir(), "ảnh kiểm thử.png")
	file, err := os.Create(path)
	if err != nil {
		t.Fatalf("create temporary PNG: %v", err)
	}
	if err := png.Encode(file, img); err != nil {
		file.Close()
		t.Fatalf("encode temporary PNG: %v", err)
	}
	if err := file.Close(); err != nil {
		t.Fatalf("close temporary PNG: %v", err)
	}

	var stdout, stderr bytes.Buffer
	if exitCode := run([]string{path}, &stdout, &stderr); exitCode != 0 {
		t.Fatalf("run exit code = %d, stderr = %q", exitCode, stderr.String())
	}
	for _, expected := range []string{
		"Định dạng: png",
		"Kích thước: 24 x 24",
		"Laplacian variance:",
		"Trạng thái:",
	} {
		if !strings.Contains(stdout.String(), expected) {
			t.Errorf("output does not contain %q:\n%s", expected, stdout.String())
		}
	}
}

func verticalStripes(width, height, stripeWidth int) *image.Gray {
	img := image.NewGray(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			if (x/stripeWidth)%2 == 0 {
				img.SetGray(x, y, color.Gray{Y: 20})
			} else {
				img.SetGray(x, y, color.Gray{Y: 235})
			}
		}
	}
	return img
}

func ycbcrStripes(width, height, stripeWidth int) *image.YCbCr {
	img := image.NewYCbCr(image.Rect(0, 0, width, height), image.YCbCrSubsampleRatio420)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			value := uint8(20)
			if (x/stripeWidth)%2 != 0 {
				value = 235
			}
			img.Y[img.YOffset(x, y)] = value
		}
	}
	for i := range img.Cb {
		img.Cb[i] = 128
		img.Cr[i] = 128
	}
	return img
}

func boxBlur(src *image.Gray, radius int) *image.Gray {
	bounds := src.Bounds()
	dst := image.NewGray(bounds)
	for y := bounds.Min.Y; y < bounds.Max.Y; y++ {
		for x := bounds.Min.X; x < bounds.Max.X; x++ {
			var sum, count int
			for offsetY := -radius; offsetY <= radius; offsetY++ {
				py := clamp(y+offsetY, bounds.Min.Y, bounds.Max.Y-1)
				for offsetX := -radius; offsetX <= radius; offsetX++ {
					px := clamp(x+offsetX, bounds.Min.X, bounds.Max.X-1)
					sum += int(src.GrayAt(px, py).Y)
					count++
				}
			}
			dst.SetGray(x, y, color.Gray{Y: uint8(sum / count)})
		}
	}
	return dst
}

func clamp(value, minimum, maximum int) int {
	if value < minimum {
		return minimum
	}
	if value > maximum {
		return maximum
	}
	return value
}

var benchmarkResult Result

func BenchmarkBlurDetection(b *testing.B) {
	const width, height = 1536, 2048
	img := ycbcrStripes(width, height, 8)

	b.Run("AllocatingFullFrameFloat64", func(b *testing.B) {
		b.ReportAllocs()
		b.SetBytes(int64(width * height))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			benchmarkResult = analyzeAllocatingBaseline(img, DefaultThresholds)
		}
		b.ReportMetric(float64(width*height*8), "working-B")
	})

	b.Run("ReusableRollingRowsUint16", func(b *testing.B) {
		detector, err := NewDetector(DetectorConfig{
			ExpectedWidth:  max(width, height),
			ReservePercent: DefaultReservePercent,
			Thresholds:     DefaultThresholds,
		})
		if err != nil {
			b.Fatalf("NewDetector returned an error: %v", err)
		}
		if _, err := detector.Analyze(img); err != nil {
			b.Fatalf("warm-up Analyze returned an error: %v", err)
		}
		b.ReportAllocs()
		b.SetBytes(int64(width * height))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			benchmarkResult, err = detector.Analyze(img)
			if err != nil {
				b.Fatal(err)
			}
		}
		b.ReportMetric(float64(detector.BufferBytes()), "working-B")
	})
}

// analyzeAllocatingBaseline preserves the original implementation for tests
// and benchmarks only. Production code never calls this function.
func analyzeAllocatingBaseline(img image.Image, thresholds Thresholds) Result {
	bounds := img.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	bitDepth := sourceBitDepth(img)
	gray := make([]float64, width*height)
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			gray[y*width+x] = float64(grayscaleValue(img.At(bounds.Min.X+x, bounds.Min.Y+y), bitDepth))
		}
	}

	score := varianceOfLaplacianAllocating(gray, width, height)
	return Result{
		Score:    score,
		Status:   Classify(score, thresholds),
		Width:    width,
		Height:   height,
		BitDepth: bitDepth,
	}
}

func varianceOfLaplacianAllocating(gray []float64, width, height int) float64 {
	var count int
	var mean, m2 float64
	for y := 0; y < height; y++ {
		above := reflect101(y-1, height)
		below := reflect101(y+1, height)
		for x := 0; x < width; x++ {
			left := reflect101(x-1, width)
			right := reflect101(x+1, width)
			cornerSum := gray[above*width+left] +
				gray[above*width+right] +
				gray[below*width+left] +
				gray[below*width+right]
			laplacian := 2*cornerSum - 8*gray[y*width+x]

			count++
			delta := laplacian - mean
			mean += delta / float64(count)
			m2 += delta * (laplacian - mean)
		}
	}
	return m2 / float64(count)
}
