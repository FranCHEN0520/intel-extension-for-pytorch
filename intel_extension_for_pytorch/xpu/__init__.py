# -*- coding: utf-8 -*-
r"""
This package is lazily initialized, so you can always import it.
"""
import ctypes
from functools import lru_cache
import sys
from typing import List, Optional, Tuple, Union, Dict

import torch
import intel_extension_for_pytorch

from torch import serialization
from torch.storage import _StorageBase, _LegacyStorage, _warn_typed_storage_removal
from torch import device as _device
from torch._utils import classproperty, _get_device_index

from .lazy_init import (
    _lazy_init,
    _lazy_call,
    _is_initialized,
    is_initialized,
    _is_in_bad_fork,
)
from .streams import Stream, Event
from .intrinsic import *
from .cpp_extension import *
from .amp import *
from .utils import *
from .deterministics import *
from .random import *
from .memory import *
from ..utils.channels_last_1d import is_contiguous_channels_last_1d, to_channels_last_1d

from .overrides import (
    override_tensor_totype,
    override_assert_equal,
)

import intel_extension_for_pytorch.optim as optim
from intel_extension_for_pytorch._version import (
    __version__,
    __ipex_gitrev__,
    __torch_gitrev__,
    __gpu_onednn_gitrev__,
    __build_type__,
)

default_generators: Tuple[torch._C.Generator] = ()
_device_t = Union[_device, str, int]


def _is_compiled() -> bool:
    r"""Returns true if compile with XPU support."""
    return hasattr(intel_extension_for_pytorch._C, "_getDeviceCount")


if _is_compiled():
    _exchange_device = intel_extension_for_pytorch._C._exchangeDevice
else:

    def _exchange_device(device: int) -> int:
        if device < 0:
            return -1
        raise RuntimeError(
            "Intel® Extension for PyTorch* was compiled without XPU support"
        )


_maybe_exchange_device = _exchange_device


def init():
    r"""Initialize the XPU's state. This is a Python API about lazy initialization
    that avoids initializing XPU until the first time it is accessed. You may need
    to call this function explicitly in very rare cases, since IPEX could call
    this initialization automatically when XPU functionality is on-demand.

    Does nothing if call this function repeatedly.
    """
    _lazy_init()


def _raw_device_count() -> int:
    status, count = intel_extension_for_pytorch._C._prefetchDeviceCount()
    if status != 0:
        return -1
    return count


# This API call _raw_device_count() if _lazy_init() has not been called such
# that this API can be used before forking a child process.
@lru_cache(maxsize=1)
def device_count() -> int:
    r"""Returns the number of XPUs device available."""
    if not _is_compiled():
        return 0
    if _is_initialized():
        return intel_extension_for_pytorch._C._getDeviceCount()
    else:
        count = _raw_device_count()
        return intel_extension_for_pytorch._C._getDeviceCount() if count < 0 else count


# This API can be used before forking process if _lazy_init() has not been called.
def is_available() -> bool:
    r"""Returns a bool indicating if XPU is currently available."""
    # This function device_count() never throws and returns 0 if driver is missing
    # or can't be initialized
    return device_count() > 0


class _DeviceGuard:
    def __init__(self, index: int):
        self.idx = index
        self.prev_idx = -1

    def __enter__(self):
        self.prev_idx = torch.xpu._exchange_device(self.idx)

    def __exit__(self, type: Any, value: Any, traceback: Any):
        torch.xpu._maybe_exchange_device(self.prev_idx)
        return False


class device(object):
    r"""Context-manager that changes the selected device.

    Arguments:
        device (torch.device or int): device index to select. It's a no-op if
            this argument is a negative integer or ``None``.
    """

    def __init__(self, device: Any):
        self.idx = _get_device_index(device, optional=True)
        self.prev_idx = -1

    def __enter__(self):
        self.prev_idx = torch.xpu._exchange_device(self.idx)

    def __exit__(self, type: Any, value: Any, traceback: Any):
        torch.xpu._maybe_exchange_device(self.prev_idx)
        return False

    @property
    def sycl_device(self):
        r"""sycl_device(self): -> PyCapsule

        Returns the sycl device of the selected device in a ``PyCapsule``, which encapsules
        a void pointer address. Its capsule name is ``torch.xpu.device.sycl_device``.
        """
        return intel_extension_for_pytorch._C.sycl_device(self.idx)


class device_of(device):
    r"""Context-manager that changes the current device to that of given object.

    You can use both tensors and storages as arguments. If a given object is
    not allocated on a GPU, this is a no-op.

    Arguments:
        obj (Tensor or Storage): object allocated on the selected device.
    """

    def __init__(self, obj):
        idx = obj.get_device() if obj.is_xpu else -1
        super(device_of, self).__init__(idx)


def set_device(device: _device_t) -> None:
    r"""Sets the current device.

    Usage of this function is discouraged in favor of :any:`device`. In most
    cases it's better to use ``ZE_AFFINITY_MASK`` environmental variable to restrict
    which devices are visible.

    Arguments:
        device (torch.device or int): selected device. This function is a no-op
            if this argument is negative.
    """
    device = _get_device_index(device)
    if device >= 0:
        intel_extension_for_pytorch._C._setDevice(device)


def get_device_name(device: Optional[_device_t] = None) -> str:
    r"""Gets the name of a device.

    Arguments:
        device (torch.device or int, optional): device for which to return the
            name. This function is a no-op if this argument is a negative
            integer. It uses the current device, given by :func:`~torch.xpu.current_device`,
            if :attr:`device` is ``None`` (default).
    """
    return get_device_properties(device).name


@lru_cache(None)
def get_device_capability(device: Optional[_device_t] = None) -> Dict[str, Any]:
    r"""Gets the xpu capability of a device.

    Args:
        device (torch.device or int, optional): device for which to return the
            device capability. It uses the current device, given by
            :func:`~torch.xpu.current_device`, if :attr:`device` is ``None``
            (default).

    Returns:
        Dict[str, Any]: the xpu capability dictionary of the device
    """
    props = get_device_properties(device)
    return {
        prop: getattr(props, prop) for prop in dir(props) if not prop.startswith("__")
    }


def get_device_properties(device: _device_t):
    r"""Gets the xpu properties of a device.

    Arguments:
        device (torch.device or int, optional): device for which to return the
            device properties. It uses the current device, given by
            :func:`~torch.xpu.current_device`, if :attr:`device` is ``None``
            (default).

    Returns:
        _DeviceProperties: the properties of the device
    """
    _lazy_init()  # will define _get_device_properties
    device = _get_device_index(device, optional=True)
    if device < 0 or device >= device_count():
        raise AssertionError("Invalid device id")
    return intel_extension_for_pytorch._C._get_device_properties(device)


def current_device() -> int:
    r"""Returns the index of a currently selected device."""
    # lazy initialization occurs in _getDevice
    return intel_extension_for_pytorch._C._getDevice()


def synchronize(device: _device_t = None) -> None:
    r"""Waits for all kernels in all streams on a XPU device to complete.

    Arguments:
        device (torch.device or int, optional): device for which to synchronize.
            It uses the current device, given by :func:`~torch.xpu.current_device`,
            if :attr:`device` is ``None`` (default).
    """
    _lazy_init()
    idx = _get_device_index(device, optional=True)
    return intel_extension_for_pytorch._C._synchronize(idx)


class StreamContext(object):
    r"""Context-manager that selects a given stream.

    All XPU kernels queued within its context will be enqueued on a selected
    stream.

    Args:
        Stream (Stream): selected stream. This manager is a no-op if it's
            ``None``.
    .. note:: Streams are per-device.
    """

    cur_stream: Optional["Stream"]

    def __init__(self, stream: Optional["Stream"]):
        self.stream = stream
        self.idx = _get_device_index(None, True)
        if not torch.jit.is_scripting():
            if self.idx is None:
                self.idx = -1

        self.src_prev_stream = None
        self.dst_prev_stream = None

    def __enter__(self):
        # Local cur_stream variable for type refinement
        cur_stream = self.stream
        # Return if stream is None or XPU device not available
        if cur_stream is None or self.idx == -1:
            return
        self.src_prev_stream = current_stream(None)

        # If the stream is not on the current device, then
        # set the current stream on the device
        if self.src_prev_stream.device != cur_stream.device:
            with device(cur_stream.device):
                self.dst_prev_stream = current_stream(cur_stream.device)
        set_stream(cur_stream)

    def __exit__(self, type: Any, value: Any, traceback: Any):
        # Local cur_stream variable for type refinement
        cur_stream = self.stream
        # If stream is None or no XPU device available, return
        if cur_stream is None or self.idx == -1:
            return

        # Reset the stream on the original device
        # and destination device
        if self.src_prev_stream.device != cur_stream.device:
            set_stream(self.dst_prev_stream)
        set_stream(self.src_prev_stream)


def stream(stream: Optional["Stream"]) -> StreamContext:
    r"""Wrapper around the Context-manager StreamContext that
    selects a given stream.

    Arguments:
        stream (Stream): selected stream. This manager is a no-op if it's
            ``None``.

    .. note:: Streams are per-device. If the selected stream is not on the
        current device, this function will also change the current device to
        match the stream.
    """
    return StreamContext(stream)


def _set_stream_by_id(stream_id, device_index, device_type):
    r"""set stream specified by the stream id, device index and device type

    Args:
        stream_id (int): not visible to the user, used to assigned to the
            specific stream.
        device_index (int): selected device index.
        device_type (int): selected device type.
    """
    intel_extension_for_pytorch._C._setCurrentStream(
        stream_id=stream_id,
        device_index=device_index,
        device_type=device_type,
    )


def set_stream(stream: Stream):
    r"""Sets the current stream.This is a wrapper API to set the stream.
        Usage of this function is discouraged in favor of the ``stream``
        context manager.

    Args:
        stream (Stream): selected stream. This function is a no-op
            if this argument is ``None``.
    """
    if stream is None:
        return
    _set_stream_by_id(
        stream_id=stream.stream_id,
        device_index=stream.device_index,
        device_type=stream.device_type,
    )


def current_stream(device: Optional[_device_t] = None) -> Stream:
    r"""Returns the currently selected :class:`Stream` for a given device.

    Arguments:
        device (torch.device or int, optional): selected device. Returns
            the currently selected :class:`Stream` for the current device, given
            by :func:`~torch.xpu.current_device`, if :attr:`device` is ``None``
            (default).
    """
    _lazy_init()
    streamdata = intel_extension_for_pytorch._C._getCurrentStream(
        _get_device_index(device, optional=True)
    )
    return Stream(
        stream_id=streamdata[0], device_index=streamdata[1], device_type=streamdata[2]
    )


def _get_device(device: Union[int, str, torch.device]) -> torch.device:
    r"""Return the torch.device type object from the passed in device.

    Args:
        device (torch.device or int): selected device.
    """
    if isinstance(device, str):
        device = torch.device(device)
    elif isinstance(device, int):
        device = torch.device("xpu", device)
    return device


def _get_generator(device: torch.device) -> torch._C.Generator:
    r"""Return the XPU Generator object for the given device.

    Args:
        device (torch.device): selected device.
    """

    idx = device.index
    if idx is None:
        idx = current_device()
    return torch.xpu.default_generators[idx]


def _set_rng_state_offset(
    offset: int, device: Union[int, str, torch.device] = "xpu"
) -> None:
    r"""Sets the random number generator state offset of the specified GPU.

    Args:
        offset (int): The desired offset
        device (torch.device or int, optional): The device to set the RNG state.
            Default: ``'xpu'`` (i.e., ``torch.device('xpu')``, the current XPU device).
    """
    final_device = _get_device(device)

    def cb():
        default_generator = _get_generator(final_device)
        default_generator.set_offset(offset)

    _lazy_call(cb)


def _get_rng_state_offset(device: Union[int, str, torch.device] = "xpu") -> int:
    r"""Returns the random number generator state offset of the specified GPU.

    Args:
        device (torch.device or int, optional): The device to return the RNG state offset of.
            Default: ``'xpu'`` (i.e., ``torch.device('xpu')``, the current XPU device).

    .. warning::
        This function eagerly initializes XPU.
    """
    _lazy_init()
    final_device = _get_device(device)
    default_generator = _get_generator(final_device)
    return default_generator.get_offset()


@staticmethod  # type: ignore[misc]
def _lazy_new(cls, *args, **kwargs):
    _lazy_init()
    # We may need to call lazy init again if we are a forked child
    # del _XPUBase.__new__
    return super(_XPUBase, cls).__new__(cls, *args, **kwargs)


class _XPUBase(object):
    is_xpu = True
    is_sparse = False

    def type(self, *args, **kwargs):
        # We could use a Protocol here to tell mypy that self has `get_device` method
        # but it is only available in the typing module on Python >= 3.8
        # or on typing_extensions module on Python >= 3.6
        with device(self.get_device()):  # type: ignore[attr-defined]
            return super(_XPUBase, self).type(*args, **kwargs)  # type: ignore[misc]

    __new__ = _lazy_new


class _XPULegacyStorage(_LegacyStorage):
    @classmethod
    def from_buffer(cls, *args, **kwargs):
        _warn_typed_storage_removal()
        raise RuntimeError("from_buffer: Not available for XPU storage")

    @classmethod
    def _new_with_weak_ptr(cls, *args, **kwargs):
        raise RuntimeError("_new_with_weak_ptr: Not available for XPU storage")

    @classmethod
    def _new_shared_filename(cls, manager, obj, size, *, device=None, dtype=None):
        raise RuntimeError("_new_shared_filename: Not available for XPU storage")


class ByteStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.uint8


class DoubleStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.double


class FloatStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.float


class HalfStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.half


class LongStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.long


class IntStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.int


class ShortStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.short


class CharStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.int8


class BoolStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.bool


class BFloat16Storage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.bfloat16


class ComplexDoubleStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.cdouble


class ComplexFloatStorage(_XPULegacyStorage):
    @classproperty
    def dtype(self):
        _warn_typed_storage_removal()
        return self._dtype

    @classproperty
    def _dtype(self):
        return torch.cfloat


del _LegacyStorage
del _XPULegacyStorage

torch._storage_classes.add(DoubleStorage)
torch._storage_classes.add(FloatStorage)
torch._storage_classes.add(LongStorage)
torch._storage_classes.add(IntStorage)
torch._storage_classes.add(ShortStorage)
torch._storage_classes.add(CharStorage)
torch._storage_classes.add(ByteStorage)
torch._storage_classes.add(HalfStorage)
torch._storage_classes.add(BoolStorage)
torch._storage_classes.add(BFloat16Storage)
torch._storage_classes.add(ComplexDoubleStorage)
torch._storage_classes.add(ComplexFloatStorage)


def _xpu_tag(obj):
    if obj.device.type == "xpu":
        return "xpu:" + str(obj.device.index)


def validate_xpu_device(location):
    device = _get_device_index(location, True)
    if not torch.xpu.is_available():
        raise RuntimeError(
            "Attempting to deserialize object on a xpu "
            "device but torch.xpu.is_available() is False. "
            "If you are running on a CPU-only machine, "
            "please use torch.load with map_location=torch.device('cpu') "
            "to map your storages to the CPU."
        )
    device_count = torch.xpu.device_count()
    if device >= device_count:
        raise RuntimeError(
            "Attempting to deserialize object on xpu device "
            f"{device} but torch.xpu.device_count() is {device_count}. Please use "
            "torch.load with map_location to map your storages "
            "to an existing device."
        )
    return device


current_module = sys.modules[__name__]


def _xpu(self, device=None, non_blocking=False, **kwargs):
    """Returns a copy of this object in xpu memory.

    If this object is already in xpu memory and on the correct device, then
    no copy is performed and the original object is returned.

    Args:
        device (int): The destination GPU id. Defaults to the current device.
        non_blocking (bool): If ``True`` and the source is in pinned memory,
            the copy will be asynchronous with respect to the host. Otherwise,
            the argument has no effect.
        **kwargs: For compatibility, may contain the key ``async`` in place of
            the ``non_blocking`` argument.
    """
    non_blocking = torch._utils._get_async_or_non_blocking("xpu", non_blocking, kwargs)
    # if self.is_xpu:
    #     if device is None:
    #         device = torch.xpu.current_device()
    #     if self.get_device() == device:
    #         return self
    # else:
    if device is None:
        device = -1
    with torch.xpu.device(device):
        if self.is_sparse:
            # new_type = getattr(torch.xpu.sparse, self.__class__.__name__)
            # indices = torch._indices(self).xpu(device, non_blocking)
            # values = torch._values(self).xpu(device, non_blocking)
            # return new_type(indices, values, self.size())
            pass
        else:
            untyped_storage = torch.UntypedStorage(
                self.size(), device=torch.device("xpu")
            )
            untyped_storage.copy_(self, non_blocking)
            return untyped_storage


def _xpu_deserialize(obj, location):
    if location.startswith("xpu"):
        device_id = validate_xpu_device(location)
        if getattr(obj, "_torch_load_uninitialized", False):
            with torch.xpu.device(device):
                return torch.UntypedStorage(obj.nbytes(), device=torch.device(location))
        else:
            return _xpu(obj, device=device_id)


def get_device_type() -> str:
    return "xpu"


_StorageBase.xpu = _xpu

serialization.register_package(30, _xpu_tag, _xpu_deserialize)

torch._register_device_module("xpu", current_module)

# post initial
if hasattr(intel_extension_for_pytorch._C, "_postInitExtension"):
    intel_extension_for_pytorch._C._postInitExtension()

if intel_extension_for_pytorch._C._has_xpu():
    if is_available():
        if not has_fp64_dtype():
            override_tensor_totype()

            exec_path = sys.argv[0].split("/")
            if len(exec_path) > 0 and "pytest" in exec_path:
                override_assert_equal()


def _prepare_profiler(config, activities):
    # global profiler need to trigger lazy init
    _lazy_init()
    return intel_extension_for_pytorch._C._prepare_profiler(config, activities)


if "torch.autograd.profiler" in sys.modules:
    mod = sys.modules["torch.autograd.profiler"]
    mod._prepare_profiler = _prepare_profiler
else:
    import torch.autograd.profiler

    mod = sys.modules["torch.autograd.profiler"]
    mod._prepare_profiler = _prepare_profiler
