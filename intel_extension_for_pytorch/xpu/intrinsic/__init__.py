import torch
from torch.nn.modules.utils import _pair
from torch import nn, Tensor
from torch.jit.annotations import BroadcastingList2
from typing import List, Union
from .modules import EMA
from .modules import TransducerLoss, clip_grad_norm_, clip_grad_norm
import intel_extension_for_pytorch

__all__ = [
    "TransducerLoss",
    "nms",
    "locations_to_boxes",
    "roi_align",
    "IpexSDP",
    "IpexSDP_Index",
    "EMA",
    "clip_grad_norm_",
    "clip_grad_norm",
]


def MulAdd(input, other, accumu, alpha=1.0):
    return torch.ops.torch_ipex.mul_add(input, other, accumu, alpha)


def nms(dets, scores, iou_threshold):
    return torch.ops.torch_ipex.nms(dets, scores, iou_threshold)


def locations_to_boxes(locations, priors, center_variance, size_variance):
    return torch.ops.torch_ipex.locations_to_boxes(
        locations, priors, center_variance, size_variance
    )


def check_roi_boxes_shape(boxes: Union[Tensor, List[Tensor]]):
    if isinstance(boxes, (list, tuple)):
        for _tensor in boxes:
            torch._assert(
                _tensor.size(1) == 4,
                "The shape of the tensor in the boxes list is not correct as List[Tensor[L, 4]]",
            )
    elif isinstance(boxes, torch.Tensor):
        torch._assert(
            boxes.size(1) == 5, "The boxes tensor shape is not correct as Tensor[K, 5]"
        )
    else:
        torch._assert(
            False, "boxes is expected to be a Tensor[L, 5] or a List[Tensor[K, 4]]"
        )
    return


def convert_boxes_to_roi_format(boxes: List[Tensor]) -> Tensor:
    concat_boxes = _cat(list(boxes), dim=0)
    temp = []
    for i, b in enumerate(boxes):
        temp.append(torch.full_like(b[:, :1], i))
    ids = _cat(temp, dim=0)
    rois = torch.cat([ids, concat_boxes], dim=1)
    return rois


def roi_align(
    input: Tensor,
    boxes: Union[Tensor, List[Tensor]],
    output_size: BroadcastingList2[int],
    spatial_scale: float = 1.0,
    sampling_ratio: int = -1,
    aligned: bool = False,
) -> Tensor:
    check_roi_boxes_shape(boxes)
    rois = boxes
    output_size = _pair(output_size)
    if not isinstance(rois, torch.Tensor):
        rois = convert_boxes_to_roi_format(rois)
    return torch.ops.torch_ipex.roi_align(
        input,
        rois,
        spatial_scale,
        output_size[0],
        output_size[1],
        sampling_ratio,
        aligned,
    )


def IpexSDP(
    query,
    key,
    value,
    alibi=None,
    bias=None,
    head_mask=None,
    alpha=1.0,
    beta=1.0,
    dropout_p=0.0,
    is_causal=False,
    seq_last=False,
) -> Tensor:
    return torch.ops.torch_ipex.xetla_fsdp_forward_atten_mask_alibi_strided(
        query,
        key,
        value,
        alibi,
        bias,
        head_mask,
        alpha,
        beta,
        dropout_p,
        is_causal,
        seq_last,
    )


def IpexSDP_Index(
    query,
    key,
    value,
    key_cache,
    value_cache,
    index,
    alibi,
    attn_mask,
    head_mask,
    timestep,
    alpha,
    beta,
    dropout_p=0.0,
    is_causal=False,
) -> Tensor:
    return torch.ops.torch_ipex.xetla_fsdp_index_forward(
        query,
        key,
        value,
        key_cache,
        value_cache,
        index,
        alibi,
        attn_mask,
        head_mask,
        timestep,
        alpha,
        beta,
        dropout_p,
        is_causal,
    )
