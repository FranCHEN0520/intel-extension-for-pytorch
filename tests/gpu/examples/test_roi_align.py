# -*- coding: utf-8 -*-

import torch
from torch.testing._internal.common_utils import TestCase
import math
import numpy as np
import intel_extension_for_pytorch  # noqa


def bilinear_interpolate(data, y, x, snap_border=False):
    height, width = data.shape
    if snap_border:
        if -1 < y <= 0:
            y = 0
        elif height - 1 <= y < height:
            y = height - 1
        if -1 < x <= 0:
            x = 0
        elif width - 1 <= x < width:
            x = width - 1
    y_low = int(math.floor(y))
    x_low = int(math.floor(x))
    y_high = y_low + 1
    x_high = x_low + 1
    wy_h = y - y_low
    wx_h = x - x_low
    wy_l = 1 - wy_h
    wx_l = 1 - wx_h
    val = 0
    for wx, xp in zip((wx_l, wx_h), (x_low, x_high)):
        for wy, yp in zip((wy_l, wy_h), (y_low, y_high)):
            if 0 <= yp < height and 0 <= xp < width:
                val += wx * wy * data[yp, xp]
    return val


def expected_fn(
    in_data,
    rois,
    pool_h,
    pool_w,
    spatial_scale=1,
    sampling_ratio=-1,
    aligned=False,
    device=None,
    dtype=torch.float64,
):
    if device is None:
        device = torch.device("cpu")
    n_channels = in_data.size(1)
    out_data = torch.zeros(
        rois.size(0), n_channels, pool_h, pool_w, dtype=dtype, device=device
    )
    offset = 0.5 if aligned else 0.0
    for r, roi in enumerate(rois):
        batch_idx = int(roi[0])
        j_begin, i_begin, j_end, i_end = (
            x.item() * spatial_scale - offset for x in roi[1:]
        )
        roi_h = i_end - i_begin
        roi_w = j_end - j_begin
        bin_h = roi_h / pool_h
        bin_w = roi_w / pool_w
        for i in range(0, pool_h):
            start_h = i_begin + i * bin_h
            grid_h = sampling_ratio if sampling_ratio > 0 else int(np.ceil(bin_h))
            for j in range(0, pool_w):
                start_w = j_begin + j * bin_w
                grid_w = sampling_ratio if sampling_ratio > 0 else int(np.ceil(bin_w))
                for channel in range(0, n_channels):
                    val = 0
                    for iy in range(0, grid_h):
                        y = start_h + (iy + 0.5) * bin_h / grid_h
                        for ix in range(0, grid_w):
                            x = start_w + (ix + 0.5) * bin_w / grid_w
                            val += bilinear_interpolate(
                                in_data[batch_idx, channel, :, :],
                                y,
                                x,
                                snap_border=True,
                            )
                    val /= grid_h * grid_w
                    out_data[r, channel, i, j] = val
    return out_data


class TestNNMethod(TestCase):
    def roi_align_forward_(self, dtype_):
        device = torch.device("xpu")
        x_dtype = dtype_
        rois_dtype = dtype_
        pool_size = 5
        n_channels = 2 * (pool_size**2)
        x = torch.rand(2, n_channels, 10, 10, dtype=x_dtype, device=device)
        rois = torch.tensor(
            [
                [0, 0, 0, 9, 9],
                [0, 0, 5, 4, 9],
                [0, 5, 5, 9, 9],
                [1, 0, 0, 9, 9],
            ],  # format is (xyxy)
            dtype=rois_dtype,
            device=device,
        )
        pool_h, pool_w = pool_size, pool_size
        y = torch.xpu.roi_align(
            x, rois, [pool_h, pool_w], spatial_scale=1, sampling_ratio=-1
        )
        assert y.dtype == x.dtype
        gt_y = expected_fn(
            x,
            rois,
            pool_h,
            pool_w,
            spatial_scale=1,
            sampling_ratio=-1,
            device=device,
            dtype=x_dtype,
        )
        tol = 1e-2 if (x_dtype is torch.half or rois_dtype is torch.half) else 1e-5
        torch.testing.assert_close(gt_y.cpu(), y.cpu(), rtol=tol, atol=tol)

    def roi_align_autocast_forward_(self, dtype_):
        device = torch.device("xpu")
        pool_size = 5
        n_channels = 2 * (pool_size**2)
        x = torch.rand(2, n_channels, 10, 10, dtype=dtype_, device=device)
        rois = torch.tensor(
            [
                [0, 0, 0, 9, 9],
                [0, 0, 5, 4, 9],
                [0, 5, 5, 9, 9],
                [1, 0, 0, 9, 9],
            ],  # format is (xyxy)
            dtype=torch.float,
            device=device,
        )
        pool_h, pool_w = pool_size, pool_size

        with torch.xpu.amp.autocast(enabled=True, dtype=dtype_):
            y = torch.xpu.roi_align(
                x, rois, [pool_h, pool_w], spatial_scale=1, sampling_ratio=-1
            )
            gt_y = expected_fn(
                x,
                rois,
                pool_h,
                pool_w,
                spatial_scale=1,
                sampling_ratio=-1,
                device=device,
                dtype=torch.float,
            )
            tol = 1e-2 if dtype_ is torch.float16 else 1e-1
            torch.testing.assert_close(
                gt_y.cpu(), y.to(torch.float).cpu(), rtol=tol, atol=tol
            )

    def test_roi_align_forward(self):
        for dtype in [torch.float, torch.half]:
            print("testing dtype:", dtype)
            self.roi_align_forward_(dtype)
        for dtype in [torch.float16, torch.bfloat16]:
            print("testing dtype in autocast: ", dtype)
            self.roi_align_autocast_forward_(dtype)
