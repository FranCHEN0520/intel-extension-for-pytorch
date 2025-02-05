"""FP8 utilies for IPEX"""

from contextlib import contextmanager
from typing import Optional, Dict, Any, Tuple

import torch
from .recipe import DelayedScaling, Format
import intel_extension_for_pytorch._isa_help as ipex

_FP8_ENABLED = False
_FP8_RECIPE = None


def get_default_fp8_recipe() -> DelayedScaling:
    return DelayedScaling()


def get_fp8_dtype(fp8_recipe: DelayedScaling, fprop_tensor: bool = True) -> str:
    if fp8_recipe.fp8_format == Format.E4M3 or (
        fp8_recipe.fp8_format == Format.HYBRID and fprop_tensor
    ):
        return ipex.Float8Format.kFloat8_E4M3  # "E4M3"
    return ipex.Float8Format.kFloat8_E5M2  # "E5M2"


def is_fp8_enabled() -> bool:
    return _FP8_ENABLED


def get_fp8_recipe() -> DelayedScaling:
    return _FP8_RECIPE


@torch.jit.script
def update_amax_history(amax_history: torch.Tensor) -> torch.Tensor:
    """Update amax history and set next amax to zero."""
    if amax_history.shape[0] > 1:
        amax_history = torch.roll(amax_history, -1, 0)
    amax_history[0].fill_(0.0)
    return amax_history


@torch.jit.script
def _default_get_amax(
    amax_history: torch.Tensor,
    amax_compute_algo: str,
) -> Tuple[torch.Tensor, torch.Tensor]:
    """Default function to obtain amax from history."""
    if amax_compute_algo == "max":
        amax = torch.max(amax_history, dim=0).values
    else:
        amax = amax_history[0]

    amax_history = update_amax_history(amax_history)
    return amax_history, amax


@torch.jit.script
def _default_sf_compute(
    amax: torch.Tensor,
    scale: torch.Tensor,
    fp8_max: float,
    margin: int,
) -> torch.Tensor:
    """Default function to convert amax to scaling factor."""
    exp = torch.floor(torch.log2(fp8_max / amax)) - margin
    sf = torch.round(torch.pow(2, torch.abs(exp)))
    sf = torch.where(amax > 0.0, sf, scale)
    sf = torch.where(torch.isfinite(amax), sf, scale)
    sf = torch.where(exp < 0, 1 / sf, sf)
    return sf


@torch.jit.script
def default_amax_and_scale_update(
    amax_history: torch.Tensor,
    scale: torch.Tensor,
    fp8_max: float,
    margin: int,
    amax_compute_algo: str,
) -> Tuple[torch.Tensor, torch.Tensor]:
    """Amax to scale conversion."""

    # Get amax from history.
    amax_history, amax = _default_get_amax(
        amax_history,
        amax_compute_algo,
    )

    # Calculate new scaling factor.
    return amax_history, _default_sf_compute(
        amax,
        scale,
        fp8_max,
        margin,
    )


def amax_and_scale_update(fp8_meta: Dict[str, Any], fwd_update: bool) -> None:
    """Updates fp8 amaxes/scales for fwd | bwd."""
    fp8_meta_tensor_key = "scaling_fwd" if fwd_update else "scaling_bwd"
    fp8_max_key = "fp8_max_fwd" if fwd_update else "fp8_max_bwd"

    (amax_history, scale) = default_amax_and_scale_update(
        fp8_meta[fp8_meta_tensor_key].amax_history,
        fp8_meta[fp8_meta_tensor_key].scale,
        fp8_meta[fp8_max_key],
        fp8_meta["recipe"].margin,
        fp8_meta["recipe"].amax_compute_algo,
    )


@contextmanager
def fp8_autocast(
    enabled: bool = False,
    fp8_recipe: Optional[DelayedScaling] = None,
) -> None:
    global _FP8_ENABLED, _FP8_RECIPE
    fp8_state = (_FP8_ENABLED, _FP8_RECIPE)
    try:
        _FP8_ENABLED = enabled
        _FP8_RECIPE = get_default_fp8_recipe() if fp8_recipe is None else fp8_recipe
        yield
    finally:
        _FP8_ENABLED, _FP8_RECIPE = fp8_state
