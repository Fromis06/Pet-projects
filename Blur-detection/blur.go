package main

import (
	"errors"
	"fmt"
	"image"
	"image/color"
	"math"
)

// Status is the blur severity assigned to an image.
type Status string

const (
	StatusNormal  Status = "Bình thường"
	StatusWarning Status = "Cảnh báo"
	StatusRisk    Status = "Nguy cơ"
	StatusCheck   Status = "Cần kiểm tra"
)

const (
	DefaultReservePercent = 15
	rowAlignment          = 64
)

// Thresholds contains the three score boundaries used for classification.
// A lower Laplacian variance means a greater risk of blur.
type Thresholds struct {
	Warning float64
	Risk    float64
	Check   float64
}

// DefaultThresholds are starting values for 8-bit images. They should be
// calibrated with images captured by the real camera and workflow.
var DefaultThresholds = Thresholds{
	Warning: 150,
	Risk:    100,
	Check:   50,
}

// DetectorConfig controls the one-time allocation made by NewDetector.
// ExpectedWidth should be the largest expected orientation dimension when a
// camera can rotate. ReservePercent adds configurable headroom before the
// capacity is rounded up to a multiple of 64 pixels.
type DetectorConfig struct {
	ExpectedWidth  int
	ReservePercent int
	Thresholds     Thresholds
}

// Detector owns a reusable three-row grayscale buffer. A Detector is intended
// for one processing stream and is not safe for concurrent Analyze calls.
type Detector struct {
	thresholds    Thresholds
	capacityWidth int
	rows          []uint16
}

// Result is the score and classification produced by Analyze.
type Result struct {
	Score    float64
	Status   Status
	Width    int
	Height   int
	BitDepth int
}

// Validate checks that the thresholds describe four non-overlapping levels.
func (t Thresholds) Validate() error {
	if math.IsNaN(t.Warning) || math.IsNaN(t.Risk) || math.IsNaN(t.Check) ||
		math.IsInf(t.Warning, 0) || math.IsInf(t.Risk, 0) || math.IsInf(t.Check, 0) {
		return errors.New("các ngưỡng phải là số hữu hạn")
	}
	if t.Check < 0 {
		return errors.New("ngưỡng 'cần kiểm tra' không được âm")
	}
	if !(t.Warning > t.Risk && t.Risk > t.Check) {
		return fmt.Errorf(
			"ngưỡng phải thỏa mãn cảnh báo > nguy cơ > cần kiểm tra (hiện tại: %.2f > %.2f > %.2f)",
			t.Warning, t.Risk, t.Check,
		)
	}
	return nil
}

// NewDetector allocates the rolling buffer once. Analyze never grows it; a
// frame wider than CapacityWidth returns an error instead of allocating in the
// hot path.
func NewDetector(config DetectorConfig) (*Detector, error) {
	if config.ExpectedWidth <= 0 {
		return nil, errors.New("chiều rộng dự kiến phải lớn hơn 0")
	}
	if config.ReservePercent < 0 || config.ReservePercent > 100 {
		return nil, errors.New("phần trăm dự phòng phải nằm trong khoảng 0..100")
	}
	if err := config.Thresholds.Validate(); err != nil {
		return nil, err
	}

	requested := (int64(config.ExpectedWidth)*int64(100+config.ReservePercent) + 99) / 100
	aligned := ((requested + rowAlignment - 1) / rowAlignment) * rowAlignment
	maxInt := int64(^uint(0) >> 1)
	if aligned <= 0 || aligned > maxInt/3 {
		return nil, errors.New("kích thước buffer vượt giới hạn của hệ thống")
	}

	capacityWidth := int(aligned)
	return &Detector{
		thresholds:    config.Thresholds,
		capacityWidth: capacityWidth,
		rows:          make([]uint16, capacityWidth*3),
	}, nil
}

// CapacityWidth returns the maximum frame width accepted without allocation.
func (d *Detector) CapacityWidth() int {
	if d == nil {
		return 0
	}
	return d.capacityWidth
}

// BufferBytes returns the bytes reserved by the detector's rolling buffer.
func (d *Detector) BufferBytes() int {
	if d == nil {
		return 0
	}
	return len(d.rows) * 2
}

// Classify maps a Laplacian variance score to a severity level.
func Classify(score float64, thresholds Thresholds) Status {
	switch {
	case score >= thresholds.Warning:
		return StatusNormal
	case score >= thresholds.Risk:
		return StatusWarning
	case score >= thresholds.Check:
		return StatusRisk
	default:
		return StatusCheck
	}
}

// Analyze is a convenience function for one-off processing. Continuous frame
// processing should create one Detector and call detector.Analyze repeatedly so
// its rolling buffer is reused.
func Analyze(img image.Image, thresholds Thresholds) (Result, error) {
	if img == nil {
		return Result{}, errors.New("ảnh không được để trống")
	}
	detector, err := NewDetector(DetectorConfig{
		ExpectedWidth:  img.Bounds().Dx(),
		ReservePercent: 0,
		Thresholds:     thresholds,
	})
	if err != nil {
		return Result{}, err
	}
	return detector.Analyze(img)
}

// Analyze calculates Laplacian variance using only the preallocated rolling
// buffer. Grayscale conversion, convolution, border handling, variance and
// classification are implemented directly without an image-processing
// dependency.
func (d *Detector) Analyze(img image.Image) (Result, error) {
	if d == nil {
		return Result{}, errors.New("detector chưa được khởi tạo")
	}
	if img == nil {
		return Result{}, errors.New("ảnh không được để trống")
	}

	bounds := img.Bounds()
	width, height := bounds.Dx(), bounds.Dy()
	if width == 0 || height == 0 {
		return Result{}, errors.New("ảnh phải có ít nhất một pixel")
	}
	if width > d.capacityWidth {
		return Result{}, fmt.Errorf(
			"ảnh rộng %d pixel, vượt capacity %d; hãy tạo lại detector ngoài hot path",
			width,
			d.capacityWidth,
		)
	}

	bitDepth := sourceBitDepth(img)
	score := d.varianceOfLaplacian(img, bounds, width, height, bitDepth)
	return Result{
		Score:    score,
		Status:   Classify(score, d.thresholds),
		Width:    width,
		Height:   height,
		BitDepth: bitDepth,
	}, nil
}

// varianceOfLaplacian applies the same effective 3x3 kernel as
// cv.Laplacian(..., ksize=3):
//
//	2  0  2
//	0 -8  0
//	2  0  2
//
// Only three grayscale rows are retained. Each upcoming row overwrites a row
// that is no longer needed. BORDER_REFLECT_101 is handled at the edges, and
// Welford's algorithm calculates population variance without a Laplacian
// output buffer.
func (d *Detector) varianceOfLaplacian(
	img image.Image,
	bounds image.Rectangle,
	width, height, bitDepth int,
) float64 {
	d.fillGrayscaleRow(img, bounds, 0, width, bitDepth)

	var count int
	var mean, m2 float64
	for y := 0; y < height; y++ {
		if y+1 < height {
			d.fillGrayscaleRow(img, bounds, y+1, width, bitDepth)
		}

		aboveY := reflect101(y-1, height)
		belowY := reflect101(y+1, height)
		above := d.row(aboveY, width)
		current := d.row(y, width)
		below := d.row(belowY, width)

		for x := 0; x < width; x++ {
			left := reflect101(x-1, width)
			right := reflect101(x+1, width)
			cornerSum := int32(above[left]) +
				int32(above[right]) +
				int32(below[left]) +
				int32(below[right])
			laplacian := 2*cornerSum - 8*int32(current[x])

			count++
			value := float64(laplacian)
			delta := value - mean
			mean += delta / float64(count)
			m2 += delta * (value - mean)
		}
	}

	return m2 / float64(count)
}

func (d *Detector) row(sourceY, width int) []uint16 {
	start := (sourceY % 3) * d.capacityWidth
	return d.rows[start : start+width]
}

func (d *Detector) fillGrayscaleRow(
	img image.Image,
	bounds image.Rectangle,
	sourceY, width, bitDepth int,
) {
	destination := d.row(sourceY, width)
	y := bounds.Min.Y + sourceY

	// Standard decoders return one of these concrete image types. Reading Pix
	// directly avoids the interface allocation that img.At can cause per pixel.
	switch src := img.(type) {
	case *image.Gray:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			destination[x] = uint16(src.Pix[offset+x])
		}
	case *image.Gray16:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*2
			destination[x] = bigEndianUint16(src.Pix, pixel)
		}
	case *image.YCbCr:
		for x := 0; x < width; x++ {
			absoluteX := bounds.Min.X + x
			yOffset := src.YOffset(absoluteX, y)
			cOffset := src.COffset(absoluteX, y)
			pixel := color.YCbCr{Y: src.Y[yOffset], Cb: src.Cb[cOffset], Cr: src.Cr[cOffset]}
			r, g, b, a := pixel.RGBA()
			destination[x] = grayscalePremultipliedRGBA16(r, g, b, a, 8)
		}
	case *image.NYCbCrA:
		for x := 0; x < width; x++ {
			absoluteX := bounds.Min.X + x
			yOffset := src.YOffset(absoluteX, y)
			cOffset := src.COffset(absoluteX, y)
			pixel := color.NYCbCrA{
				YCbCr: color.YCbCr{Y: src.Y[yOffset], Cb: src.Cb[cOffset], Cr: src.Cr[cOffset]},
				A:     src.A[src.AOffset(absoluteX, y)],
			}
			r, g, b, a := pixel.RGBA()
			destination[x] = grayscalePremultipliedRGBA16(r, g, b, a, 8)
		}
	case *image.RGBA:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*4
			destination[x] = grayscalePremultipliedRGBA16(
				uint32(src.Pix[pixel])*257,
				uint32(src.Pix[pixel+1])*257,
				uint32(src.Pix[pixel+2])*257,
				uint32(src.Pix[pixel+3])*257,
				8,
			)
		}
	case *image.NRGBA:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*4
			if src.Pix[pixel+3] == 0 {
				destination[x] = 0
				continue
			}
			destination[x] = grayscaleRGB8(src.Pix[pixel], src.Pix[pixel+1], src.Pix[pixel+2])
		}
	case *image.RGBA64:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*8
			destination[x] = grayscalePremultipliedRGBA16(
				uint32(bigEndianUint16(src.Pix, pixel)),
				uint32(bigEndianUint16(src.Pix, pixel+2)),
				uint32(bigEndianUint16(src.Pix, pixel+4)),
				uint32(bigEndianUint16(src.Pix, pixel+6)),
				16,
			)
		}
	case *image.NRGBA64:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*8
			if bigEndianUint16(src.Pix, pixel+6) == 0 {
				destination[x] = 0
				continue
			}
			destination[x] = grayscaleRGB16(
				bigEndianUint16(src.Pix, pixel),
				bigEndianUint16(src.Pix, pixel+2),
				bigEndianUint16(src.Pix, pixel+4),
			)
		}
	case *image.CMYK:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			pixel := offset + x*4
			r, g, b := color.CMYKToRGB(
				src.Pix[pixel],
				src.Pix[pixel+1],
				src.Pix[pixel+2],
				src.Pix[pixel+3],
			)
			destination[x] = grayscaleRGB8(r, g, b)
		}
	case *image.Paletted:
		offset := src.PixOffset(bounds.Min.X, y)
		for x := 0; x < width; x++ {
			destination[x] = grayscaleValue(src.Palette[src.Pix[offset+x]], 8)
		}
	default:
		for x := 0; x < width; x++ {
			destination[x] = grayscaleValue(img.At(bounds.Min.X+x, y), bitDepth)
		}
	}
}

func bigEndianUint16(pixels []uint8, offset int) uint16 {
	return uint16(pixels[offset])<<8 | uint16(pixels[offset+1])
}

func grayscaleRGB8(red, green, blue uint8) uint16 {
	value := math.Floor(
		0.299*float64(red) +
			0.587*float64(green) +
			0.114*float64(blue) +
			0.5,
	)
	return uint16(value)
}

func grayscaleRGB16(red, green, blue uint16) uint16 {
	value := math.Floor(
		0.299*float64(red) +
			0.587*float64(green) +
			0.114*float64(blue) +
			0.5,
	)
	return uint16(value)
}

func grayscalePremultipliedRGBA16(red, green, blue, alpha uint32, bitDepth int) uint16 {
	if alpha == 0 {
		return 0
	}
	if alpha < 0xffff {
		red = unpremultiply(red, alpha)
		green = unpremultiply(green, alpha)
		blue = unpremultiply(blue, alpha)
	}
	redValue, greenValue, blueValue := float64(red), float64(green), float64(blue)
	if bitDepth == 8 {
		redValue /= 257
		greenValue /= 257
		blueValue /= 257
	}
	value := math.Floor(0.299*redValue + 0.587*greenValue + 0.114*blueValue + 0.5)
	return uint16(value)
}

// grayscaleValue converts one pixel using ITU-R BT.601 weights. Native 16-bit
// values remain on the 0..65535 scale instead of being reduced to 8-bit.
func grayscaleValue(pixel color.Color, bitDepth int) uint16 {
	switch c := pixel.(type) {
	case color.Gray:
		return uint16(c.Y)
	case color.Gray16:
		return c.Y
	}

	r, g, b, a := straightRGBA16(pixel)
	if a == 0 {
		return 0
	}
	red, green, blue := float64(r), float64(g), float64(b)
	if bitDepth == 8 {
		red /= 257
		green /= 257
		blue /= 257
	}
	value := math.Floor(0.299*red + 0.587*green + 0.114*blue + 0.5)
	return uint16(value)
}

func sourceBitDepth(img image.Image) int {
	switch img.(type) {
	case *image.Gray16, *image.RGBA64, *image.NRGBA64:
		return 16
	default:
		return 8
	}
}

// straightRGBA16 returns non-premultiplied RGB channels on the 0..65535 scale.
func straightRGBA16(c color.Color) (uint32, uint32, uint32, uint32) {
	r, g, b, a := c.RGBA()
	if a > 0 && a < 0xffff {
		r = unpremultiply(r, a)
		g = unpremultiply(g, a)
		b = unpremultiply(b, a)
	}
	return r, g, b, a
}

func unpremultiply(channel, alpha uint32) uint32 {
	value := (uint64(channel)*0xffff + uint64(alpha)/2) / uint64(alpha)
	if value > 0xffff {
		return 0xffff
	}
	return uint32(value)
}

// reflect101 mirrors coordinates without repeating the outermost pixel:
// gfedcb|abcdefgh|gfedcba. This is OpenCV's BORDER_DEFAULT behavior.
func reflect101(position, length int) int {
	if length <= 1 {
		return 0
	}
	for position < 0 || position >= length {
		if position < 0 {
			position = -position
		} else {
			position = 2*length - position - 2
		}
	}
	return position
}
