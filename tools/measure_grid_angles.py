"""Measure actual grid line directions in a selected camera-frame ROI; never synthesize frames.

Requires OpenCV and numpy. x,y,w,h specifies an ROI in the supplied image's pixels.
Output angles are observed scene angles, including physical roll and perspective, NOT isolated software roll.
"""
import argparse
import json
import math
import cv2
import numpy as np


def measure(path, roi):
    frame = cv2.imread(str(path))
    if frame is None:
        raise ValueError(f"Cannot read {path}")
    x, y, w, h = roi
    gray = cv2.cvtColor(frame[y:y+h, x:x+w], cv2.COLOR_BGR2GRAY)
    gray = cv2.GaussianBlur(gray, (3, 3), 0)
    lines = cv2.HoughLinesP(cv2.Canny(gray, 50, 130), 1, math.pi / 18000, 60,
                            minLineLength=min(w, h) * .35, maxLineGap=15)
    families = {"horizontal": [], "vertical": []}
    if lines is not None:
        for x1, y1, x2, y2 in lines.reshape(-1, 4):
            angle = math.degrees(math.atan2(y2-y1, x2-x1))
            residual = (angle + 45) % 90 - 45
            if abs(residual) > 10:
                continue
            family = "horizontal" if abs(x2-x1) > abs(y2-y1) else "vertical"
            families[family].append(residual)
    return {"image": str(path), "roi": roi, **{
        family: {"line_count": len(angles),
                 "median_degrees": round(float(np.median(angles)), 4) if angles else None,
                 "p10_degrees": round(float(np.percentile(angles, 10)), 4) if angles else None,
                 "p90_degrees": round(float(np.percentile(angles, 90)), 4) if angles else None}
        for family, angles in families.items()}}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("--roi", type=int, nargs=4, required=True)
    args = parser.parse_args()
    print(json.dumps(measure(args.image, args.roi), indent=2))
