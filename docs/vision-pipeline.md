# 컴퓨터비전 처리 흐름

기존 C++ 영상처리 소스의 일부를 발췌했습니다. 모델 파일, 영상, 실제 인식 문자열은 포함하지 않습니다. 발췌 코드는 문맥 설명용이며 공개 CMake의 실행 대상이 아닙니다.

## 검출과 OCR을 별도 단계로 연결

원본 `edge_plate_ocr.cpp`의 `process_frame()`은 detector 결과를 confidence 기준으로 정렬한 뒤 후보 수를 제한하고, 각 검출 박스를 crop하여 OCR에 넘깁니다. 아래는 같은 함수의 연속 구간 두 개입니다. 중간의 출력 디렉터리 생성 및 뒤쪽의 이미지·결과 저장은 생략했습니다.

```cpp
    const auto detector_started = Clock::now();
    std::vector<Detection> detections = detector.detect(frame);
    const double detector_ms = elapsed_ms(detector_started);
    if (frame_timing != nullptr) {
        frame_timing->frames_processed += 1;
        frame_timing->detector_ms += detector_ms;
        frame_timing->detector_raw_count += static_cast<int>(detections.size());
    }
    std::sort(detections.begin(), detections.end(), [](const Detection& a, const Detection& b) {
        return a.score > b.score;
    });
    if (args.max_detections_per_frame > 0 &&
        detections.size() > static_cast<size_t>(args.max_detections_per_frame)) {
        detections.resize(static_cast<size_t>(args.max_detections_per_frame));
    }
    if (frame_timing != nullptr) {
        frame_timing->detector_used_count += static_cast<int>(detections.size());
    }
```

```cpp
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < detections.size(); ++i) {
        const cv::Rect rect = crop_rect(detections[i].box, frame.size(), args.crop_margin_x, args.crop_margin_y);
        if (rect.width <= 0 || rect.height <= 0) {
            continue;
        }
        const cv::Mat crop = frame(rect).clone();
        const auto ocr_started = Clock::now();
        const OcrResult ocr = recognizer.recognize(crop);
        if (frame_timing != nullptr) {
            frame_timing->ocr_runs += 1;
            frame_timing->ocr_ms += elapsed_ms(ocr_started);
        }
}
```

마지막 닫는 중괄호는 발췌한 반복 구간을 읽기 쉽게 닫기 위해 추가했습니다. 실제 함수 전체를 복제한 것은 아닙니다.

## 영상 경계에 맞춰 crop

박스 주위에 여유를 두되 프레임 범위를 벗어나지 않도록 제한합니다. 원본 `crop_rect()` 함수입니다.

```cpp
cv::Rect crop_rect(const cv::Rect2f& box, const cv::Size& size, double margin_x_ratio, double margin_y_ratio) {
    const int margin_x = static_cast<int>(std::round(std::max(8.0, static_cast<double>(box.width) * margin_x_ratio)));
    const int margin_y = static_cast<int>(std::round(std::max(6.0, static_cast<double>(box.height) * margin_y_ratio)));
    const int x1 = std::clamp(static_cast<int>(std::round(box.x)) - margin_x, 0, size.width);
    const int y1 = std::clamp(static_cast<int>(std::round(box.y)) - margin_y, 0, size.height);
    const int x2 = std::clamp(static_cast<int>(std::round(box.x + box.width)) + margin_x, 0, size.width);
    const int y2 = std::clamp(static_cast<int>(std::round(box.y + box.height)) + margin_y, 0, size.height);
    return cv::Rect(cv::Point(x1, y1), cv::Point(std::max(x1, x2), std::max(y1, y2)));
}
```

## OCR 입력 전처리

OCR 모델 입력 높이에 맞춰 가로세로 비율을 유지하고, 입력 폭을 넘지 않도록 제한합니다. 채널별 데이터를 배열에 옮기고 정규화합니다. 원본 `OcrRecognizer::preprocess()`입니다.

```cpp
    std::vector<float> preprocess(const cv::Mat& crop_bgr) const {
        if (crop_bgr.empty()) {
            throw std::runtime_error("empty OCR crop");
        }
        const int resized_width = std::min(
            width_,
            std::max(1, static_cast<int>(std::ceil(static_cast<double>(height_) * crop_bgr.cols / crop_bgr.rows))));
        cv::Mat resized;
        cv::resize(crop_bgr, resized, cv::Size(resized_width, height_), 0, 0, cv::INTER_LINEAR);
        std::vector<float> out(static_cast<size_t>(3 * height_ * width_), 0.0F);
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < height_; ++y) {
                const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
                for (int x = 0; x < resized_width; ++x) {
                    const float value = static_cast<float>(row[x][c]) / 255.0F;
                    out[static_cast<size_t>(c * height_ * width_ + y * width_ + x)] =
                        (value - 0.5F) / 0.5F;
                }
            }
        }
        return out;
    }
```

이후 `recognize()`가 ONNX Runtime으로 추론하고, 시간축의 blank·연속 중복을 제거해 문자 후보를 복원합니다. confidence를 모아 반환하지만 이 값은 실측 정확도가 아닙니다. 결과의 형식 검사와 허용 목록 판단은 후속 단계입니다.

## ROI와 현장 제약

원본 `edge_gateway.cpp`의 관심영역 mask 생성 함수입니다. 진입 구간을 정한 뒤 해당 영역의 변화량을 확인합니다.

```cpp
cv::Mat make_polygon_mask(const cv::Size& size, const std::vector<cv::Point2f>& normalized) {
    cv::Mat mask = cv::Mat::zeros(size, CV_8UC1);
    const auto points = scale_polygon(normalized, size.width, size.height);
    cv::fillPoly(mask, std::vector<std::vector<cv::Point>>{points}, cv::Scalar(255));
    return mask;
}
```

ROI 설정으로 도로 전체를 처리 대상으로 삼는 부담을 줄였습니다. 이 영역 제한을 야간 인식 해결이나 정확도 향상 수치로 연결하지 않습니다. 조도·모션 블러에 대한 한계는 [검증 및 현재 한계](limitations.md)에 별도로 기록했습니다.

발췌 기준: 2026-10-10 로컬 영상처리 구현. 운영 설정과 실제 데이터는 사용하지 않았습니다.
