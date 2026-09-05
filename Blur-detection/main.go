package main

import (
	"flag"
	"fmt"
	"image"
	_ "image/gif"
	_ "image/jpeg"
	_ "image/png"
	"io"
	"os"
)

func main() {
	os.Exit(run(os.Args[1:], os.Stdout, os.Stderr))
}

func run(args []string, stdout, stderr io.Writer) int {
	flags := flag.NewFlagSet("blur-detector", flag.ContinueOnError)
	flags.SetOutput(stderr)

	warning := flags.Float64("warning", DefaultThresholds.Warning, "ngưỡng bắt đầu mức bình thường")
	risk := flags.Float64("risk", DefaultThresholds.Risk, "ngưỡng bắt đầu mức cảnh báo")
	check := flags.Float64("check", DefaultThresholds.Check, "ngưỡng bắt đầu mức nguy cơ")
	reserve := flags.Int("reserve", DefaultReservePercent, "phần trăm chiều rộng cấp phát dự phòng")
	flags.Usage = func() {
		fmt.Fprintln(stderr, "Cách dùng: blur-detector [tùy chọn] <đường-dẫn-ảnh>")
		fmt.Fprintln(stderr, "")
		fmt.Fprintln(stderr, "Định dạng hỗ trợ: JPEG, PNG và GIF (khung hình đầu tiên).")
		fmt.Fprintln(stderr, "Tùy chọn:")
		flags.PrintDefaults()
	}

	if err := flags.Parse(args); err != nil {
		return 2
	}
	if flags.NArg() != 1 {
		flags.Usage()
		return 2
	}

	thresholds := Thresholds{
		Warning: *warning,
		Risk:    *risk,
		Check:   *check,
	}
	if err := thresholds.Validate(); err != nil {
		fmt.Fprintf(stderr, "Ngưỡng không hợp lệ: %v\n", err)
		return 2
	}

	path := flags.Arg(0)
	img, format, err := decodeImage(path)
	if err != nil {
		fmt.Fprintf(stderr, "Không thể đọc ảnh: %v\n", err)
		return 1
	}

	bounds := img.Bounds()
	expectedWidth := max(bounds.Dx(), bounds.Dy())
	detector, err := NewDetector(DetectorConfig{
		ExpectedWidth:  expectedWidth,
		ReservePercent: *reserve,
		Thresholds:     thresholds,
	})
	if err != nil {
		fmt.Fprintf(stderr, "Không thể khởi tạo detector: %v\n", err)
		return 1
	}

	result, err := detector.Analyze(img)
	if err != nil {
		fmt.Fprintf(stderr, "Không thể phân tích ảnh: %v\n", err)
		return 1
	}

	fmt.Fprintf(stdout, "Ảnh: %s\n", path)
	fmt.Fprintf(stdout, "Định dạng: %s\n", format)
	fmt.Fprintf(stdout, "Kích thước: %d x %d\n", result.Width, result.Height)
	fmt.Fprintf(stdout, "Độ sâu xử lý: %d-bit\n", result.BitDepth)
	fmt.Fprintf(stdout, "Buffer làm việc: %.2f KiB (capacity %d pixel)\n", float64(detector.BufferBytes())/1024, detector.CapacityWidth())
	fmt.Fprintf(stdout, "Laplacian variance: %.4f\n", result.Score)
	fmt.Fprintf(stdout, "Trạng thái: %s\n", result.Status)

	if result.BitDepth == 16 && thresholds == DefaultThresholds {
		fmt.Fprintln(stdout, "Lưu ý: ảnh 16-bit giữ nguyên dải giá trị; hãy hiệu chỉnh ngưỡng cho dữ liệu 16-bit.")
	}
	return 0
}

func decodeImage(path string) (image.Image, string, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, "", err
	}
	defer file.Close()

	img, format, err := image.Decode(file)
	if err != nil {
		return nil, "", err
	}
	return img, format, nil
}
